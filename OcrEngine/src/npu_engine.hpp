#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <ze_api.h>
#include <ze_graph_ext.h>
#include <ze_graph_profiling_ext.h>

// After the Level Zero headers: it rebinds each entry point to a pointer that
// is resolved from ze_loader.dll on first use.
#include "ze_runtime.hpp"

#include "profiler.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ppocr {

struct TensorDesc {
    std::string name;
    std::vector<uint32_t> dims;
    size_t total_bytes = 0;
    bool is_input = true;
};

struct NpuGraphLoadStats {
    double graph_init_ms = 0.0;
    double profiling_blob_export_ms = 0.0;
    double argument_query_ms = 0.0;
    double request_pool_ms = 0.0;
    double total_ms = 0.0;
};

// Shared queue pool slot. Request index -> pool index; at most one in-flight
// request per index (det slot i%2, rec slot b%depth, grid slice s%2), so one
// slot serves all graphs and the pool stays tiny.
struct QueueSlot {
    ze_command_queue_handle_t queue = nullptr;
    ze_fence_handle_t fence = nullptr;
    uint32_t ordinal = 0;
};

// Represents a single asynchronous inference request slot. Owns its command
// list and USM buffers; the command queue and fence come from the engine pool.
class NpuInferRequest {
public:
    NpuInferRequest(
        ze_context_handle_t context,
        ze_device_handle_t device,
        QueueSlot* slot,
        ze_graph_handle_t graph,
        const std::vector<TensorDesc>& inputs,
        const std::vector<TensorDesc>& outputs,
        ze_graph_dditable_ext_t* graph_api,
        ze_graph_profiling_dditable_ext_t* profiling_api = nullptr,
        TraceRecorder* trace = nullptr,
        std::string graph_name = {},
        size_t request_index = 0
    );
    ~NpuInferRequest();

    void* get_input_ptr(size_t index = 0) const { return input_ptrs_[index]; }
    void* get_output_ptr(size_t index = 0) const { return output_ptrs_[index]; }
    size_t get_input_size(size_t index = 0) const { return input_descs_[index].total_bytes; }
    size_t get_output_size(size_t index = 0) const { return output_descs_[index].total_bytes; }

    // Submits the inference task asynchronously to NPU command queue
    void start_async();

    // Waits for NPU execution to complete (blocking)
    void wait();

    // Non-blocking completion check
    bool try_wait();

private:
    ze_context_handle_t context_;
    ze_device_handle_t device_;
    ze_command_queue_handle_t queue_;
    ze_graph_handle_t graph_;
    ze_graph_dditable_ext_t* graph_api_;
    ze_graph_profiling_dditable_ext_t* profiling_api_ = nullptr;
    TraceRecorder* trace_ = nullptr;
    std::string graph_name_;
    size_t request_index_ = 0;

    std::vector<TensorDesc> input_descs_;
    std::vector<TensorDesc> output_descs_;

    std::vector<void*> input_ptrs_;
    std::vector<void*> output_ptrs_;

    ze_command_list_handle_t command_list_ = nullptr;
    ze_fence_handle_t fence_ = nullptr;
    ze_graph_profiling_pool_handle_t profiling_pool_ = nullptr;
    ze_graph_profiling_query_handle_t profiling_query_ = nullptr;
    uint64_t* command_timestamp_before_ = nullptr;
    uint64_t* command_timestamp_after_ = nullptr;
    uint64_t command_before_tick_ = 0;
    uint64_t command_after_tick_ = 0;
    TraceRecorder::TimePoint submit_start_{};
    TraceRecorder::TimePoint submit_end_{};
    bool in_flight_ = false;

    void collect_profile(TraceRecorder::TimePoint completion);
    void destroy_profile_query();
    void record_command_timestamps();
};

// Manages a compiled model instance on NPU
class NpuGraph {
public:
    NpuGraph(
        ze_context_handle_t context,
        ze_device_handle_t device,
        std::vector<QueueSlot>* queue_pool,
        const std::string& model_path,
        ze_graph_dditable_ext_t* graph_api,
        size_t num_requests = 2,       // in-flight software pipeline depth per graph
        bool use_lite = false,         // load .ngraph_lite through driver JIT
        std::span<const int64_t> fill_shape = {}, // concrete dims for 4-digit shape templates
        bool manual_blob_cache = false, // disable driver cache, keep blobs in cache/lite/
        std::filesystem::path cache_dir = {},
        ze_graph_profiling_dditable_ext_t* profiling_api = nullptr,
        TraceRecorder* trace = nullptr,
        std::string expected_sha256 = {}
    );
    ~NpuGraph();

    // Acquires an available infer request from the pool
    NpuInferRequest& get_request(size_t slot = 0) { return *requests_[slot % requests_.size()]; }
    size_t request_count() const { return requests_.size(); }

    const std::vector<TensorDesc>& inputs() const { return inputs_; }
    const std::vector<TensorDesc>& outputs() const { return outputs_; }
    const NpuGraphLoadStats& load_stats() const { return load_stats_; }

private:
    void init_graph(const std::string& model_path);
    void initialize_graph();
    void query_arguments();

    // Manual blob cache: compiled NGRAPH_LITE graphs are exported with
    // pfnGetNativeBinary2 and reloaded as NATIVE blobs (driver cache disabled).
    bool load_manual_blob_cache(const std::string& blob_path, bool fresh, const std::string& name);
    bool store_manual_blob_cache(const std::string& blob_path, const std::string& name);
    // Creates a NATIVE graph directly from a read-only mmap of `path`. The
    // driver copies the blob during graph init, so the mapping is released as
    // soon as the graph is initialized (no pinned pages, no private vector).
    bool try_create_mapped(const std::string& path, const std::string& name);

    ze_context_handle_t context_;
    ze_device_handle_t device_;
    std::vector<QueueSlot>* queue_pool_ = nullptr;
    ze_graph_dditable_ext_t* graph_api_;
    ze_graph_handle_t graph_ = nullptr;

    std::vector<TensorDesc> inputs_;
    std::vector<TensorDesc> outputs_;
    std::vector<std::unique_ptr<NpuInferRequest>> requests_;
    bool use_lite_ = false;
    std::vector<int64_t> fill_shape_;
    bool manual_blob_cache_ = false;
    std::filesystem::path cache_dir_ = L"cache";
    ze_graph_profiling_dditable_ext_t* profiling_api_ = nullptr;
    TraceRecorder* trace_ = nullptr;
    std::string expected_sha256_;
    std::string graph_name_;
    NpuGraphLoadStats load_stats_;
};

// Singleton context managing the Intel NPU Level Zero driver
class NpuEngine {
public:
    static NpuEngine& instance();

    bool initialize();

    // Loads the .ngraph_lite container through the driver's JIT compiler; the
    // driver's disk cache is reused.
    void set_ngraph_lite(bool enabled) { use_lite_ = enabled; }

    // Skips the driver-managed disk cache (ZE_GRAPH_FLAG_DISABLE_CACHING) and
    // keeps compiled blobs in the project's own cache/lite/ directory instead.
    void set_manual_blob_cache(bool enabled) { manual_blob_cache_ = enabled; }

    void set_cache_dir(std::filesystem::path path) {
        cache_dir_ = path.empty() ? std::filesystem::path(L"cache") : std::move(path);
    }

    // Opt-in low-latency command queues (ZE_extension_command_queue_npu): the
    // kernel/firmware maximize NPU performance for jobs on these queues (no
    // DVFS downclocking after idle) and the UMD busy-waits on completion.
    // Costs power; firmware marks it "not recommended for regular usage".
    // Must be set before the queue pool is created (i.e. before load_model()).
    void set_turbo(bool enabled) { turbo_ = enabled; }

    void set_trace_recorder(TraceRecorder* trace);

    // Prints active NPU/context allocation usage. Cache statistics are separate
    // because querying them can scan driver-managed cache state.
    std::optional<uint64_t> print_memory_usage(const char* tag = "");

    // Prints driver/program cache statistics. This is diagnostic-only and is
    // intentionally not part of the normal inference profile.
    void print_cache_usage(const char* tag = "");

    std::shared_ptr<NpuGraph> load_model(const std::string& model_path,
                                         size_t num_requests = 2,
                                         std::span<const int64_t> fill_shape = {},
                                         const std::string& expected_sha256 = {});

    ze_context_handle_t context() const { return context_; }
    ze_device_handle_t device() const { return device_; }
    ze_graph_dditable_ext_t* graph_api() const { return graph_api_; }

    const std::string& device_name() const { return device_name_; }

private:
    NpuEngine() = default;
    ~NpuEngine();

    // Grows the shared queue pool to at least `count` slots
    void ensure_queue_pool(size_t count);

    bool initialized_ = false;
    ze_driver_handle_t driver_ = nullptr;
    ze_device_handle_t device_ = nullptr;
    ze_context_handle_t context_ = nullptr;
    uint32_t queue_ordinal_ = 0;
    std::vector<QueueSlot> queue_pool_;
    ze_graph_dditable_ext_t* graph_api_ = nullptr;
    ze_graph_profiling_dditable_ext_t* profiling_api_ = nullptr;
    bool use_lite_ = false;
    bool manual_blob_cache_ = false;
    std::filesystem::path cache_dir_ = L"cache";
    bool turbo_ = false;
    bool turbo_supported_ = false;
    bool turbo_warned_ = false;
    TraceRecorder* trace_ = nullptr;
    std::string device_name_ = "Unknown NPU";
};

} // namespace ppocr