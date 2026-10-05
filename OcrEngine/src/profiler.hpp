#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ze_api.h>
#include <ze_graph_ext.h>
#include <ze_graph_profiling_ext.h>

namespace ppocr {

struct TraceNpuTask {
    std::string name;
    std::string layer_type;
    std::string executor;
    uint64_t start_ns = 0;
    uint64_t duration_ns = 0;
    uint32_t active_cycles = 0;
    uint32_t stall_cycles = 0;
    uint32_t task_id = 0;
};

class TraceRecorder {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TraceRecorder() : origin_(Clock::now()) {}

    void set_raw_dump_directory(std::string path) {
        raw_dump_directory_ = std::move(path);
        if (!raw_dump_directory_.empty()) std::filesystem::create_directories(raw_dump_directory_);
    }

    bool has_raw_dump_directory() const { return !raw_dump_directory_.empty(); }

    void set_task_profiling_enabled(bool enabled) { task_profiling_enabled_ = enabled; }
    bool task_profiling_enabled() const { return task_profiling_enabled_; }

    void dump_native_graph(ze_graph_dditable_ext_t* api, ze_graph_handle_t graph, std::string name) const {
        if (raw_dump_directory_.empty() || !api || !api->pfnGetNativeBinary2 || !graph) return;
        size_t size = 0;
        const uint8_t* data = nullptr;
        const ze_result_t result = api->pfnGetNativeBinary2(graph, &size, &data);
        if (result != ZE_RESULT_SUCCESS || !data || size == 0) {
            std::cerr << "[!] Could not export profiling graph blob for raw trace (0x" << std::hex << result
                      << std::dec << "): " << name << "\n";
            return;
        }
        std::ofstream out(raw_dump_path(std::move(name), ".blob"), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out) throw std::runtime_error("failed to write profiling graph blob");
    }

    void add_global_timestamp_sample(std::string label, uint64_t host_ns, uint64_t device_ticks) {
        std::lock_guard lock(mutex_);
        clock_samples_.push_back({std::move(label), host_ns, 0, device_ticks});
    }

    void add_global_timestamp_sample_with_steady(std::string label, uint64_t api_host_ns,
                                                  uint64_t steady_ns, uint64_t device_ticks) {
        std::lock_guard lock(mutex_);
        clock_samples_.push_back({std::move(label), api_host_ns, steady_ns, device_ticks});
    }

    void add_command_timestamp_sample(std::string label, uint64_t device_ticks) {
        add_global_timestamp_sample(std::move(label), 0, device_ticks);
    }

    void set_device_timer_properties(uint64_t resolution, uint32_t timestamp_bits) {
        device_timer_resolution_ = resolution;
        device_timestamp_bits_ = timestamp_bits;
    }

    void add_host_event(std::string_view name, std::string_view category,
                        TimePoint start, TimePoint end, int track = 1) {
        std::lock_guard lock(mutex_);
        host_events_.push_back({std::string(name), std::string(category),
                                micros(start), micros(end) - micros(start), track});
    }

    void add_npu_submission(std::string graph, TimePoint submit_start, TimePoint submit_end,
                            TimePoint completion, std::vector<TraceNpuTask> tasks) {
        std::lock_guard lock(mutex_);
        submissions_.push_back({std::move(graph), nanos(submit_start), nanos(submit_end),
                                nanos(completion), 0, 0, std::move(tasks)});
    }

    void add_pending_profile(ze_graph_profiling_dditable_ext_t* api,
                             ze_graph_profiling_pool_handle_t pool,
                             ze_graph_profiling_query_handle_t query,
                             std::string graph, TimePoint submit_start,
                             TimePoint submit_end, TimePoint completion,
                             uint64_t command_before_tick,
                             uint64_t command_after_tick) {
        std::lock_guard lock(mutex_);
        pending_profiles_.push_back({api, pool, query, std::move(graph), nanos(submit_start),
                                     nanos(submit_end), nanos(completion), command_before_tick,
                                     command_after_tick, next_profile_id_++});
    }

    size_t collect_pending_profiles() {
        std::vector<PendingProfile> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(pending_profiles_);
        }

        for (auto& profile : pending) {
            const auto read_start = Clock::now();
            std::vector<TraceNpuTask> tasks;
            if (!raw_dump_directory_.empty() && profile.api && profile.query &&
                profile.api->pfnProfilingQueryGetData) {
                uint32_t raw_size = 0;
                ze_result_t raw_result = profile.api->pfnProfilingQueryGetData(
                    profile.query, ZE_GRAPH_PROFILING_RAW, &raw_size, nullptr);
                if (raw_result == ZE_RESULT_SUCCESS && raw_size > 0) {
                    std::vector<uint8_t> raw_data(raw_size);
                    raw_result = profile.api->pfnProfilingQueryGetData(
                        profile.query, ZE_GRAPH_PROFILING_RAW, &raw_size, raw_data.data());
                    if (raw_result == ZE_RESULT_SUCCESS) {
                        std::ofstream raw_out(raw_dump_path(
                            std::to_string(profile.profile_id) + "-" + profile.graph, ".raw"),
                            std::ios::binary | std::ios::trunc);
                        raw_out.write(reinterpret_cast<const char*>(raw_data.data()), raw_size);
                        if (!raw_out) throw std::runtime_error("failed to write raw NPU profile buffer");
                    }
                }
                if (raw_result != ZE_RESULT_SUCCESS) {
                    std::cerr << "[!] Raw NPU profile read failed (0x" << std::hex << raw_result
                              << std::dec << ") for " << profile.graph << ".\n";
                }
            }
            if (profile.api && profile.query && profile.api->pfnProfilingQueryGetData) {
                uint32_t size = 0;
                ze_result_t result = profile.api->pfnProfilingQueryGetData(
                    profile.query, ZE_GRAPH_PROFILING_TASK_LEVEL, &size, nullptr);
                if (result == ZE_RESULT_SUCCESS && size >= sizeof(ze_profiling_task_info)) {
                    std::vector<uint8_t> data(size);
                    result = profile.api->pfnProfilingQueryGetData(
                        profile.query, ZE_GRAPH_PROFILING_TASK_LEVEL, &size, data.data());
                    if (result == ZE_RESULT_SUCCESS) {
                        const auto* info = reinterpret_cast<const ze_profiling_task_info*>(data.data());
                        const size_t count = size / sizeof(ze_profiling_task_info);
                        tasks.reserve(count);
                        auto copy_name = [](const char* value, size_t capacity) {
                            const char* end = std::find(value, value + capacity, '\0');
                            return std::string(value, end);
                        };
                        auto executor_name = [](ze_task_execute_type_t type) -> const char* {
                            switch (type) {
                            case ZE_TASK_EXECUTE_DPU: return "DPU";
                            case ZE_TASK_EXECUTE_SW: return "SW";
                            case ZE_TASK_EXECUTE_DMA: return "DMA";
                            default: return "Other";
                            }
                        };
                        for (size_t i = 0; i < count; ++i) {
                            tasks.push_back({copy_name(info[i].name, sizeof(info[i].name)),
                                             copy_name(info[i].layer_type, sizeof(info[i].layer_type)),
                                             executor_name(info[i].exec_type), info[i].start_time_ns,
                                             info[i].duration_ns, info[i].active_cycles,
                                             info[i].stall_cycles, info[i].task_id});
                        }
                    } else {
                        std::cerr << "[!] Deferred NPU trace task read failed (0x" << std::hex << result
                                  << std::dec << ").\n";
                    }
                } else if (result != ZE_RESULT_SUCCESS) {
                    std::cerr << "[!] Deferred NPU trace task query failed (0x" << std::hex << result
                              << std::dec << ").\n";
                }
            }

            if (profile.api) {
                if (profile.query && profile.api->pfnProfilingQueryDestroy)
                    profile.api->pfnProfilingQueryDestroy(profile.query);
                if (profile.pool && profile.api->pfnProfilingPoolDestroy)
                    profile.api->pfnProfilingPoolDestroy(profile.pool);
            }

            const auto read_end = Clock::now();
            std::lock_guard lock(mutex_);
            submissions_.push_back({std::move(profile.graph), profile.submit_start_ns,
                                    profile.submit_end_ns, profile.completion_ns,
                                    profile.command_before_tick, profile.command_after_tick,
                                    std::move(tasks)});
            host_events_.push_back({"Deferred task profile read", "Profiler Overhead",
                                    micros(read_start), micros(read_end) - micros(read_start), 8});
        }
        if (!raw_dump_directory_.empty()) {
            write_clock_samples();
            write_device_timer_properties();
        }
        return pending.size();
    }

    bool write_chrome_trace(const std::string& path, std::string& error) const {
        std::lock_guard lock(mutex_);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot open trace output";
            return false;
        }

        std::vector<ExportEvent> events;
        events.reserve(host_events_.size() + 64);
        for (const auto& event : host_events_) {
            events.push_back({event.name, event.category, event.start_us, event.duration_us,
                              event.track, "{}"});
        }

        bool has_device_tasks = false;
        uint64_t first_submit_ns = std::numeric_limits<uint64_t>::max();
        uint64_t last_completion_ns = 0;
        uint64_t fallback_device_cursor_ns = 0;
        std::vector<std::pair<double, double>> active_intervals;
        std::vector<std::pair<double, double>> command_windows;
        std::map<std::string, std::vector<double>> executor_lane_ends;
        std::map<int, std::string> npu_track_names;
        std::vector<double> command_lane_ends;
        const auto calibration = device_tick_calibration();
        std::vector<const Submission*> ordered;
        ordered.reserve(submissions_.size());
        for (const auto& submission : submissions_) ordered.push_back(&submission);
        std::sort(ordered.begin(), ordered.end(), [](const Submission* a, const Submission* b) {
            return a->submit_start_ns < b->submit_start_ns;
        });
        for (const Submission* submission_ptr : ordered) {
            const auto& submission = *submission_ptr;
            first_submit_ns = std::min(first_submit_ns, submission.submit_start_ns);
            last_completion_ns = std::max(last_completion_ns, submission.completion_ns);
            events.push_back({submission.graph + " submit", "NPU Submit",
                              relative_us(submission.submit_start_ns),
                              relative_us(submission.submit_end_ns) - relative_us(submission.submit_start_ns),
                              2, "{}"});
            uint64_t first_task_ns = std::numeric_limits<uint64_t>::max();
            uint64_t last_task_ns = 0;
            for (const auto& task : submission.tasks) {
                if (task.duration_ns == 0) continue;
                first_task_ns = std::min(first_task_ns, task.start_ns);
                last_task_ns = std::max(last_task_ns, task.start_ns + task.duration_ns);
            }
            const bool has_submission_tasks = first_task_ns != std::numeric_limits<uint64_t>::max();
            std::optional<double> task_origin_us;
            std::optional<double> command_after_us;
            if (calibration && submission.command_before_tick != 0) {
                task_origin_us = map_device_tick(*calibration, submission.command_before_tick);
                if (submission.command_after_tick != 0)
                    command_after_us = map_device_tick(*calibration, submission.command_after_tick);
            }
            if (task_origin_us && command_after_us && *command_after_us >= *task_origin_us)
                command_windows.emplace_back(*task_origin_us, *command_after_us);
            if (task_origin_us && command_after_us && *command_after_us >= *task_origin_us) {
                size_t command_lane = 0;
                while (command_lane < command_lane_ends.size() &&
                       command_lane_ends[command_lane] > *task_origin_us) {
                    ++command_lane;
                }
                if (command_lane == command_lane_ends.size()) {
                    command_lane_ends.push_back(*command_after_us);
                } else {
                    command_lane_ends[command_lane] = *command_after_us;
                }
                const int command_event_track = command_track + static_cast<int>(command_lane) * 10;
                npu_track_names.try_emplace(command_event_track,
                                            "NPU command window lane " + std::to_string(command_lane));
                events.push_back({submission.graph + " device command", "NPU Command Window",
                                  *task_origin_us, *command_after_us - *task_origin_us,
                                  command_event_track, "{\"source\":\"command-list-global-timestamp\"}"});
                const double submit_end_us = relative_us(submission.submit_end_ns);
                if (*task_origin_us - submit_end_us >= min_bubble_us) {
                    events.push_back({submission.graph + " queue wait", "NPU Queue Wait",
                                      submit_end_us, *task_origin_us - submit_end_us,
                                      command_event_track, "{\"reason\":\"command-list-started-after-host-submit\"}"});
                }
            }
            if (!has_submission_tasks) continue;
            const uint64_t fallback_graph_start_ns = std::max(
                submission.submit_end_ns, fallback_device_cursor_ns);
            fallback_device_cursor_ns = fallback_graph_start_ns + (last_task_ns - first_task_ns);
            if (!task_origin_us) {
                command_windows.emplace_back(relative_us(fallback_graph_start_ns),
                                             relative_us(fallback_device_cursor_ns));
            }
            std::vector<const TraceNpuTask*> ordered_tasks;
            ordered_tasks.reserve(submission.tasks.size());
            for (const auto& task : submission.tasks) ordered_tasks.push_back(&task);
            std::sort(ordered_tasks.begin(), ordered_tasks.end(), [](const TraceNpuTask* a, const TraceNpuTask* b) {
                return a->start_ns < b->start_ns;
            });
            for (const TraceNpuTask* task_ptr : ordered_tasks) {
                const auto& task = *task_ptr;
                if (task.duration_ns == 0) continue;
                has_device_tasks = true;
                const double start_us = task_origin_us
                    ? *task_origin_us + task.start_ns / 1000.0
                    : relative_us(fallback_graph_start_ns + (task.start_ns - first_task_ns));
                const double duration_us = task.duration_ns / 1000.0;
                auto& lanes = executor_lane_ends[task.executor];
                size_t lane = 0;
                while (lane < lanes.size() && lanes[lane] > start_us) ++lane;
                if (lane == lanes.size()) lanes.push_back(start_us + duration_us);
                else lanes[lane] = start_us + duration_us;
                const int task_track = executor_track(task.executor) + static_cast<int>(lane) * 10;
                npu_track_names.try_emplace(task_track, "NPU " + task.executor + " lane " + std::to_string(lane));
                constexpr bool cycle_counters_valid = false;
                constexpr const char* counter_source = "task-level-unvalidated-counters";
                std::string args = "{\"layer\":\"" + json_escape(task.layer_type) +
                    "\",\"executor\":\"" + json_escape(task.executor) +
                    "\",\"active_cycles\":" + std::to_string(task.active_cycles) +
                    ",\"stall_cycles\":" + std::to_string(task.stall_cycles) +
                    ",\"active_cycles_valid\":" + (cycle_counters_valid ? "true" : "false") +
                    ",\"stall_cycles_valid\":" + (cycle_counters_valid ? "true" : "false") +
                    ",\"counter_source\":\"" + counter_source + "\"" +
                    ",\"task_id\":" + std::to_string(task.task_id) + "}";
                events.push_back({task.name, "NPU " + task.executor, start_us, duration_us,
                                  task_track, std::move(args)});
                active_intervals.emplace_back(start_us, start_us + duration_us);
            }
        }

        if (has_device_tasks && !command_windows.empty()) {
            std::sort(command_windows.begin(), command_windows.end());
            std::sort(active_intervals.begin(), active_intervals.end());
            for (const auto& window : command_windows) {
                double cursor = window.first;
                for (const auto& interval : active_intervals) {
                    const double start = std::max(interval.first, window.first);
                    const double end = std::min(interval.second, window.second);
                    if (end <= start || end <= cursor) continue;
                    if (start - cursor >= min_bubble_us) {
                        events.push_back({"NPU uncovered interval", "NPU Bubble", cursor, start - cursor,
                                          bubble_track, "{\"reason\":\"no TASK_LEVEL event covers this interval\",\"classification\":\"uncovered_interval\"}"});
                    }
                    cursor = std::max(cursor, end);
                }
                if (window.second - cursor >= min_bubble_us) {
                    events.push_back({"NPU uncovered interval", "NPU Bubble", cursor, window.second - cursor,
                                      bubble_track, "{\"reason\":\"no TASK_LEVEL event covers this interval\",\"classification\":\"uncovered_interval\"}"});
                }
            }
        }

        const char* task_time_source = task_profiling_enabled_
            ? "ZE_GRAPH_PROFILING_TASK_LEVEL" : "none-timestamp-only";
        const char* task_anchor = task_profiling_enabled_
            ? "command-before-device-timestamp" : "command-global-timestamp";
        out << std::setprecision(15)
            << "{\"displayTimeUnit\":\"ms\",\"timeDomain\":\"host-steady-calibrated\","
            << "\"npuTaskTimeSource\":\"" << task_time_source << "\","
            << "\"npuTaskAnchor\":\"" << task_anchor << "\","
            << "\"rawProfilingRequired\":false,\"npuSchedule\":\"device-timestamp-anchored\",\"traceEvents\":[";
        bool first = true;
        auto metadata = [&](int track, const char* name) {
            if (!first) out << ',';
            first = false;
            out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":"
                << track << ",\"args\":{\"name\":\"" << name << "\"}}";
        };
        metadata(1, "Host stages");
        metadata(2, "Submit and fence wait");
        metadata(bubble_track, "NPU bubbles");
        metadata(8, "Profiler overhead");
        metadata(2000, "Post-load pipeline envelope");
        metadata(2001, "End-to-end envelope");
        metadata(2002, "Detection submission envelope");
        metadata(2003, "Recognition pipeline envelope");
        for (const auto& [track, name] : npu_track_names) metadata(track, name.c_str());
        for (const auto& event : events) {
            if (!first) out << ',';
            first = false;
            out << "{\"name\":\"" << json_escape(event.name)
                << "\",\"cat\":\"" << json_escape(event.category)
                << "\",\"ph\":\"X\",\"ts\":" << event.start_us
                << ",\"dur\":" << event.duration_us
                << ",\"pid\":1,\"tid\":" << event.track
                << ",\"args\":" << event.args_json << '}';
        }
        out << "]}";
        if (!out) {
            error = "failed while writing trace output";
            return false;
        }
        return true;
    }

private:
    struct HostEvent {
        std::string name;
        std::string category;
        double start_us;
        double duration_us;
        int track;
    };
    struct Submission {
        std::string graph;
        uint64_t submit_start_ns;
        uint64_t submit_end_ns;
        uint64_t completion_ns;
        uint64_t command_before_tick;
        uint64_t command_after_tick;
        std::vector<TraceNpuTask> tasks;
    };
    struct PendingProfile {
        ze_graph_profiling_dditable_ext_t* api;
        ze_graph_profiling_pool_handle_t pool;
        ze_graph_profiling_query_handle_t query;
        std::string graph;
        uint64_t submit_start_ns;
        uint64_t submit_end_ns;
        uint64_t completion_ns;
        uint64_t command_before_tick;
        uint64_t command_after_tick;
        size_t profile_id;
    };
    struct ExportEvent {
        std::string name;
        std::string category;
        double start_us;
        double duration_us;
        int track;
        std::string args_json;
    };
    struct ClockSample {
        std::string label;
        uint64_t api_host_ns;
        uint64_t steady_ns;
        uint64_t device_ticks;
    };

    static constexpr int bubble_track = 6;
    static constexpr int command_track = 1000;
    static constexpr double min_bubble_us = 25.0;

    std::string raw_dump_path(std::string name, const char* extension) const {
        for (char& c : name) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
        }
        return (std::filesystem::path(raw_dump_directory_) / (name + extension)).string();
    }

    static std::string json_escape(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (unsigned char c : value) {
            switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += hex[c >> 4];
                    escaped += hex[c & 0xf];
                } else {
                    escaped += static_cast<char>(c);
                }
            }
        }
        return escaped;
    }

    int executor_track(const std::string& executor) const {
        if (executor == "DPU") return 3;
        if (executor == "SW") return 4;
        if (executor == "DMA") return 5;
        return 7;
    }

    uint64_t nanos(TimePoint point) const {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            point.time_since_epoch()).count());
    }

    double micros(TimePoint point) const {
        return std::chrono::duration<double, std::micro>(point - origin_).count();
    }

    double relative_us(uint64_t timestamp_ns) const {
        return (static_cast<int64_t>(timestamp_ns) - origin_ns_) / 1000.0;
    }

    struct DeviceTickCalibration {
        long double slope_ns_per_tick;
        long double intercept_ns;
    };

    std::optional<DeviceTickCalibration> device_tick_calibration() const {
        long double mean_tick = 0.0L;
        long double mean_steady = 0.0L;
        size_t count = 0;
        for (const auto& sample : clock_samples_) {
            if (sample.steady_ns == 0) continue;
            mean_tick += static_cast<long double>(sample.device_ticks);
            mean_steady += static_cast<long double>(sample.steady_ns);
            ++count;
        }
        if (count < 2) return std::nullopt;
        mean_tick /= static_cast<long double>(count);
        mean_steady /= static_cast<long double>(count);

        long double numerator = 0.0L;
        long double denominator = 0.0L;
        for (const auto& sample : clock_samples_) {
            if (sample.steady_ns == 0) continue;
            const long double dt = static_cast<long double>(sample.device_ticks) - mean_tick;
            const long double ds = static_cast<long double>(sample.steady_ns) - mean_steady;
            numerator += dt * ds;
            denominator += dt * dt;
        }
        if (denominator <= 0.0L) return std::nullopt;
        const long double slope = numerator / denominator;
        return DeviceTickCalibration{slope, mean_steady - slope * mean_tick};
    }

    double map_device_tick(const DeviceTickCalibration& calibration, uint64_t tick) const {
        const long double steady_ns = calibration.slope_ns_per_tick *
            static_cast<long double>(tick) + calibration.intercept_ns;
        return static_cast<double>((steady_ns - static_cast<long double>(origin_ns_)) / 1000.0L);
    }

    Clock::time_point origin_;
    int64_t origin_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        origin_.time_since_epoch()).count();
    mutable std::mutex mutex_;
    std::vector<HostEvent> host_events_;
    std::vector<Submission> submissions_;
    std::vector<PendingProfile> pending_profiles_;
    std::string raw_dump_directory_;
    bool task_profiling_enabled_ = true;
    size_t next_profile_id_ = 0;
    std::vector<ClockSample> clock_samples_;
    uint64_t device_timer_resolution_ = 0;
    uint32_t device_timestamp_bits_ = 0;

    void write_clock_samples() const {
        std::ofstream out(std::filesystem::path(raw_dump_directory_) / "global-timestamp-samples.csv",
                          std::ios::trunc);
        if (!out) throw std::runtime_error("failed to write global timestamp samples");
        out << "label,api_host_timestamp_ns,steady_timestamp_ns,device_timestamp_ticks\n";
        for (const auto& sample : clock_samples_)
            out << sample.label << ',' << sample.api_host_ns << ',' << sample.steady_ns << ','
                << sample.device_ticks << '\n';
    }


    void write_device_timer_properties() const {
        std::ofstream out(std::filesystem::path(raw_dump_directory_) / "device-timer-properties.csv",
                          std::ios::trunc);
        if (!out) throw std::runtime_error("failed to write device timer properties");
        out << "timer_resolution_cycles_per_second,timestamp_valid_bits\n"
            << device_timer_resolution_ << ',' << device_timestamp_bits_ << '\n';
    }
};

struct ProfileMetrics {
    bool enabled = false;
    TraceRecorder* trace = nullptr;
    double image_decode_ms = 0.0;
    double engine_init_ms = 0.0;
    double dict_load_ms = 0.0;
    double model_load_ms = 0.0;
    double det_model_load_ms = 0.0;
    double rec_model_load_ms = 0.0;
    double post_load_pipeline_ms = 0.0;
    double tile_prep_ms = 0.0;
    double det_preprocess_ms = 0.0;
    double det_inference_ms = 0.0;
    double det_output_decode_ms = 0.0;
    bool det_submission_wall_includes_components = false;
    int det_tile_count = 1;
    double dbnet_postprocess_ms = 0.0;
    int detected_box_count = 0;
    double rec_crop_prep_ms = 0.0;
    double rec_padding_ms = 0.0;
    double rec_preprocess_ms = 0.0;
    double rec_submit_ms = 0.0;
    double rec_wait_ms = 0.0;
    double rec_pipeline_wall_ms = 0.0;
    double ctc_decode_ms = 0.0;
    double total_end_to_end_ms = 0.0;

    static double displayed_ms(double value) {
        return std::round(value * 100.0) / 100.0;
    }

    void print_report() const {
        if (!enabled) return;
        const double shown_total = displayed_ms(total_end_to_end_ms);
        const double shown_post_load = displayed_ms(post_load_pipeline_ms);
        const double shown_image = displayed_ms(image_decode_ms);
        const double shown_runtime = displayed_ms(engine_init_ms);
        const double shown_models = displayed_ms(model_load_ms);
        const double shown_dictionary = displayed_ms(dict_load_ms);
        const double end_to_end_other = shown_total - shown_image - shown_runtime -
                                        shown_models - shown_dictionary - shown_post_load;

        const double shown_tile_prep = displayed_ms(tile_prep_ms);
        const double shown_detection = displayed_ms(det_submission_wall_includes_components
                                                        ? det_inference_ms
                                                        : det_preprocess_ms + det_inference_ms + det_output_decode_ms);
        const double shown_dbnet = displayed_ms(dbnet_postprocess_ms);
        const double shown_recognition = displayed_ms(rec_pipeline_wall_ms > 0.0
                                                          ? rec_pipeline_wall_ms
                                                          : rec_crop_prep_ms + rec_padding_ms + rec_preprocess_ms +
                                                                rec_submit_ms + rec_wait_ms + ctc_decode_ms);
        const double post_load_other = shown_post_load - shown_tile_prep - shown_detection -
                                       shown_dbnet - shown_recognition;

        std::cout << "\n============================================================\n";
        std::cout << "          End-to-End Inference Profiling Metrics           \n";
        std::cout << "============================================================\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  End-to-end additive ledger (ms)\n";
        std::cout << "    Image acquisition / WIC decode   : " << std::setw(7) << shown_image << "\n";
        std::cout << "    NPU runtime initialization      : " << std::setw(7) << shown_runtime << "\n";
        std::cout << "    NPU model/request load           : " << std::setw(7) << shown_models << "\n";
        std::cout << "    Dictionary load                  : " << std::setw(7) << shown_dictionary << "\n";
        std::cout << "    Post-load OCR pipeline           : " << std::setw(7) << shown_post_load << "\n";
        std::cout << "    Other / inter-stage time         : " << std::setw(7) << end_to_end_other << "\n";
        std::cout << "    Total end-to-end                 : " << std::setw(7) << shown_total << " ms\n";
        std::cout << "  Model load detail (included in model/request load)\n";
        std::cout << "    Detection graph load             : " << std::setw(7) << det_model_load_ms << " ms\n";
        std::cout << "    Recognition graphs load          : " << std::setw(7) << rec_model_load_ms << " ms\n";
        std::cout << "------------------------------------------------------------\n";
        std::cout << "  Post-load additive ledger (ms; sums to post-load pipeline)\n";
        std::cout << "    Tile extraction/preparation      : " << std::setw(7) << shown_tile_prep << "\n";
        std::cout << "    Detection stage wall             : " << std::setw(7)
                  << shown_detection << "\n";
        std::cout << "    DBNet polygon extraction         : " << std::setw(7) << shown_dbnet << "\n";
        if (rec_pipeline_wall_ms > 0.0) {
            std::cout << "    Recognition pipeline wall        : " << std::setw(7) << shown_recognition << "\n";
        } else {
            std::cout << "    Recognition measured components  : " << std::setw(7)
                      << shown_recognition << "\n";
        }
        std::cout << "    Other / uninstrumented work      : " << std::setw(7) << post_load_other << "\n";
        std::cout << "    Post-load OCR pipeline            : " << std::setw(7) << shown_post_load << " ms\n";
        std::cout << "  Detailed timers (diagnostic; do not sum with their enclosing wall timers)\n";
        std::cout << "    Det resize/normalize             : " << std::setw(7) << det_preprocess_ms << " ms\n";
        std::cout << "    Det submission/wait wall         : " << std::setw(7) << det_inference_ms << " ms (avg " << (det_tile_count ? det_inference_ms / det_tile_count : 0.0) << " ms/tile";
        if (det_submission_wall_includes_components)
            std::cout << "; includes resize/normalize and output decode in tiled path";
        std::cout << ")\n";
        std::cout << "    Det output FP16 decode/copy      : " << std::setw(7) << det_output_decode_ms << " ms\n";
        std::cout << "    DBNet polygon extraction         : " << std::setw(7) << dbnet_postprocess_ms << " ms (" << detected_box_count << " boxes)\n";
        std::cout << "    Text crop/perspective warp       : " << std::setw(7) << rec_crop_prep_ms << " ms\n";
        std::cout << "    Rec padding/copy                 : " << std::setw(7) << rec_padding_ms << " ms\n";
        std::cout << "    Rec normalize                    : " << std::setw(7) << rec_preprocess_ms << " ms\n";
        std::cout << "    Rec submit                       : " << std::setw(7) << rec_submit_ms << " ms\n";
        std::cout << "    Rec wait                         : " << std::setw(7) << rec_wait_ms << " ms\n";
        std::cout << "    Recognition pipeline wall        : " << std::setw(7) << rec_pipeline_wall_ms << " ms (includes recognition detail above)\n";
        std::cout << "    CTC greedy decoding              : " << std::setw(7) << ctc_decode_ms << " ms\n";
        std::cout << "------------------------------------------------------------\n";
        if (total_end_to_end_ms > 0.0) {
            std::cout << "  Page Throughput                    : " << std::setw(7) << (1000.0 / total_end_to_end_ms) << " FPS\n";
        }
        if (rec_pipeline_wall_ms > 0.0 && detected_box_count > 0) {
            std::cout << "  Recognition Pipeline Throughput    : " << std::setw(7) << (detected_box_count / (rec_pipeline_wall_ms / 1000.0)) << " lines/sec\n";
        }
        std::cout << "============================================================\n";
    }
};

class ScopedTimer {
public:
    explicit ScopedTimer(double& target_ms, TraceRecorder* trace = nullptr,
                         std::string_view label = {}, std::string_view category = "Host",
                         int track = 1)
        : target_(target_ms), trace_(trace), label_(label), category_(category), track_(track),
          start_(TraceRecorder::Clock::now()) {}
    ~ScopedTimer() {
        auto end = TraceRecorder::Clock::now();
        target_ += std::chrono::duration<double, std::milli>(end - start_).count();
        if (trace_ && !label_.empty()) trace_->add_host_event(label_, category_, start_, end, track_);
    }
private:
    double& target_;
    TraceRecorder* trace_;
    std::string_view label_;
    std::string_view category_;
    int track_;
    TraceRecorder::TimePoint start_;
};

} // namespace ppocr
