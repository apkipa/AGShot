#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ocr_engine.hpp"

#include "npu_engine.hpp"
#include "postprocess.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ppocr {

namespace {

namespace fs = std::filesystem;

// The detector graph is a fixed square bucket: Resize mode scales the image so
// its long edge fits and letterboxes it into this size.
constexpr int kDetBucket = 960;

// Recognition graphs are 48-pixel-high buckets that emit one CTC step per 8
// pixels of input width.
constexpr int kRecHeight = 48;
constexpr int kRecCellStride = 8;

// Sliding window for lines no bucket can hold: 640 pixels wide with 128 pixels
// of overlap, split down the middle so each slice owns half of the overlap.
constexpr int kLongLineWindow = 640;
constexpr int kLongLineOverlap = 128;

// A caller may pass either a ready .ngraph_lite file or its extension-less
// model base. The application ships only .ngraph_lite model containers.
std::wstring resolve_model(const std::wstring& path) {
    if (path.empty()) return {};
    std::error_code error;
    if (fs::is_regular_file(path, error) && !error) return path;

    const std::wstring candidate = path + L".ngraph_lite";
    if (fs::is_regular_file(candidate, error) && !error) return candidate;
    return {};
}

// The graph loader takes narrow paths, so a wide path is converted once, here.
std::string narrow_path(const std::wstring& path) {
    return fs::path(path).string();
}

// Det resize snaps to the 32-pixel grid the detector's downsampling expects.
int round_to_32(int value) {
    return std::max(32, static_cast<int>(std::floor(value / 32.0 + 0.5)) * 32);
}

void decode_prob_map(const uint16_t* source, int count, float* destination) {
    for (int i = 0; i < count; ++i) destination[i] = fp16_to_float(source[i]);
}

// A graph whose tensors are not exactly the ones the pipeline drives it with
// would read or write the wrong memory: the detector always fills one square
// bucket and reads back a same-size probability map, and every recognition
// bucket emits one CTC step per eight columns of input width. Each loaded graph
// is therefore checked before it is used, and a mismatch names both shapes so
// the wrong template is obvious.
std::string shape_text(const std::vector<uint32_t>& dims) {
    if (dims.empty()) return "<none>";
    std::string text = std::to_string(dims.front());
    for (size_t i = 1; i < dims.size(); ++i) {
        text += "x";
        text += std::to_string(dims[i]);
    }
    return text;
}

void require_shape(const std::string& graph, const char* role, const TensorDesc& tensor,
                   const std::vector<uint32_t>& expected_dims, size_t expected_bytes) {
    if (tensor.dims == expected_dims && tensor.total_bytes == expected_bytes) return;
    throw std::runtime_error(
        graph + " " + role + " is " + shape_text(tensor.dims) + " (" +
        std::to_string(tensor.total_bytes) + " bytes), expected " + shape_text(expected_dims) + " (" +
        std::to_string(expected_bytes) + " bytes); the model does not match this bucket");
}

void require_single_tensor(const std::string& graph, const NpuGraph& loaded) {
    if (loaded.inputs().size() == 1 && loaded.outputs().size() == 1) return;
    throw std::runtime_error(
        graph + " exposes " + std::to_string(loaded.inputs().size()) + " input(s) and " +
        std::to_string(loaded.outputs().size()) + " output(s); exactly one of each is required");
}

} // namespace

struct OcrEngine::State {
    OcrEngineOptions options;
    std::shared_ptr<NpuGraph> det_graph;
    std::shared_ptr<NpuGraph> rec_graph_160;
    std::shared_ptr<NpuGraph> rec_graph_320;
    std::shared_ptr<NpuGraph> rec_graph_640;
    std::shared_ptr<NpuGraph> rec_graph_1280;
    std::unique_ptr<CtcDecoder> ctc_decoder;
    int rec_vocab_size = 0; // dictionary size plus the CTC blank, checked against every rec graph
    bool ready = false;

    void fill_det_input(const ImageBGR& image, void* destination) const {
        if (options.det_input_u8) {
            std::memcpy(destination, image.data.data(), image.data.size());
        } else {
            WicDecoder::normalize_to_nchw_fp16(image, destination);
        }
    }

    void fill_rec_input(const ImageBGR& source, int src_x, int valid_w, int target_w, void* destination) const {
        if (options.rec_input_u8) {
            auto* dst = static_cast<uint8_t*>(destination);
            const size_t copy_bytes = static_cast<size_t>(valid_w) * 3;
            const size_t pad_bytes = static_cast<size_t>(target_w - valid_w) * 3;
            for (int y = 0; y < kRecHeight; ++y) {
                const uint8_t* src_row = source.data.data() + (static_cast<size_t>(y) * source.width + src_x) * 3;
                uint8_t* dst_row = dst + static_cast<size_t>(y) * target_w * 3;
                std::memcpy(dst_row, src_row, copy_bytes);
                if (pad_bytes > 0) {
                    std::memset(dst_row + copy_bytes, 0, pad_bytes);
                }
            }
        } else {
            ImageBGR padded_crop;
            padded_crop.width = target_w;
            padded_crop.height = kRecHeight;
            padded_crop.channels = 3;
            padded_crop.data.assign(static_cast<size_t>(target_w) * kRecHeight * 3, 0);
            const size_t copy_bytes = static_cast<size_t>(valid_w) * 3;
            for (int y = 0; y < kRecHeight; ++y) {
                std::memcpy(
                    padded_crop.data.data() + static_cast<size_t>(y) * target_w * 3,
                    source.data.data() + (static_cast<size_t>(y) * source.width + src_x) * 3,
                    copy_bytes);
            }
            WicDecoder::normalize_to_nchw_fp16(
                padded_crop, destination, 127.5f, 127.5f, 127.5f, 127.5f, 127.5f, 127.5f);
        }
    }

    void detect(const ImageBGR& image, ProbHeatmap& heatmap);
    void recognize_boxes(const ImageBGR& image, std::vector<TextBox>& boxes);
};

void OcrEngine::State::detect(const ImageBGR& image, ProbHeatmap& heatmap) {
    const int tile_size = options.det_tile_size;

    if (options.det_mode == DetMode::Resize) {
        int tw = image.width;
        int th = image.height;
        {
            const int max_side = std::max(tw, th);
            const double ratio = (max_side > kDetBucket)
                ? static_cast<double>(kDetBucket) / max_side
                : 1.0;
            tw = round_to_32(static_cast<int>(image.width * ratio));
            th = round_to_32(static_cast<int>(image.height * ratio));
        }

        auto& req = det_graph->get_request(0);
        {
            ImageBGR resized = WicDecoder::resize_linear(image, tw, th);

            ImageBGR bucket;
            bucket.width = kDetBucket;
            bucket.height = kDetBucket;
            bucket.channels = 3;
            bucket.data.assign(static_cast<size_t>(kDetBucket) * kDetBucket * 3, 0);
            for (int y = 0; y < th; ++y) {
                std::memcpy(bucket.data.data() + static_cast<size_t>(y) * kDetBucket * 3,
                            resized.data.data() + static_cast<size_t>(y) * tw * 3,
                            static_cast<size_t>(tw) * 3);
            }
            fill_det_input(bucket, req.get_input_ptr(0));
        }

        req.start_async();
        req.wait();

        const uint16_t* p_out_fp16 = reinterpret_cast<const uint16_t*>(req.get_output_ptr(0));
        ProbHeatmap full(kDetBucket, kDetBucket);
        decode_prob_map(p_out_fp16, kDetBucket * kDetBucket, full.data().data());

        heatmap = ProbHeatmap(tw, th);
        for (int y = 0; y < th; ++y) {
            std::memcpy(heatmap.data().data() + static_cast<size_t>(y) * tw,
                        full.data().data() + static_cast<size_t>(y) * kDetBucket,
                        static_cast<size_t>(tw) * sizeof(float));
        }
    } else if (image.width > tile_size || image.height > tile_size) {
        // Preserve a positive stride for small experimental tile buckets.
        const int min_overlap = std::min(80, tile_size / 2);
        const int eff_step = tile_size - min_overlap;

        const int nx = (image.width <= tile_size)
            ? 1
            : static_cast<int>(std::ceil(static_cast<float>(image.width - min_overlap) / eff_step));
        const int ny = (image.height <= tile_size)
            ? 1
            : static_cast<int>(std::ceil(static_cast<float>(image.height - min_overlap) / eff_step));

        const float step_x = (nx > 1) ? static_cast<float>(image.width - tile_size) / (nx - 1) : 0.0f;
        const float step_y = (ny > 1) ? static_cast<float>(image.height - tile_size) / (ny - 1) : 0.0f;

        struct TileTask {
            int x, y;
            ImageBGR tile_img;
        };

        std::vector<TileTask> tasks;
        for (int iy = 0; iy < ny; ++iy) {
            const int ty = (ny > 1) ? static_cast<int>(std::round(iy * step_y)) : 0;
            for (int ix = 0; ix < nx; ++ix) {
                const int tx = (nx > 1) ? static_cast<int>(std::round(ix * step_x)) : 0;
                tasks.push_back({ tx, ty, WicDecoder::extract_tile(image, tx, ty, tile_size, tile_size) });
            }
        }

        // Two-slot tile pipeline: keep submitting ahead while each slot is
        // awaited only on its own queue (device execution itself is serial)
        // Two-slot tile pipeline: merge each completed output immediately.
        // This avoids retaining and converting every tile before stitching.
        for (size_t i = 0; i < tasks.size(); ++i) {
            auto& req = det_graph->get_request(i % 2);
            if (i >= 2) {
                req.wait();
                const uint16_t* p_out_fp16 = reinterpret_cast<const uint16_t*>(req.get_output_ptr(0));
                heatmap.stitch_tile_max_fp16(p_out_fp16, tile_size, tile_size,
                                             tasks[i - 2].x, tasks[i - 2].y);
            }

            fill_det_input(tasks[i].tile_img, req.get_input_ptr(0));
            req.start_async();
        }

        for (size_t i = tasks.size(); i < tasks.size() + 2; ++i) {
            if (tasks.size() >= 1) {
                const size_t slot = i - 2;
                if (slot < tasks.size()) {
                    auto& req = det_graph->get_request(slot % 2);
                    req.wait();
                    const uint16_t* p_out_fp16 = reinterpret_cast<const uint16_t*>(req.get_output_ptr(0));
                    heatmap.stitch_tile_max_fp16(p_out_fp16, tile_size, tile_size,
                                                 tasks[slot].x, tasks[slot].y);
                }
            }
        }
    } else {
        // Single-shot detection
        const ImageBGR single_tile = WicDecoder::extract_tile(image, 0, 0, tile_size, tile_size);

        auto& req = det_graph->get_request(0);
        fill_det_input(single_tile, req.get_input_ptr(0));

        req.start_async();
        req.wait();

        const uint16_t* p_out_fp16 = reinterpret_cast<const uint16_t*>(req.get_output_ptr(0));
        heatmap.stitch_tile_max_fp16(p_out_fp16, tile_size, tile_size, 0, 0);
    }
}

void OcrEngine::State::recognize_boxes(const ImageBGR& image, std::vector<TextBox>& boxes) {
    if (boxes.empty()) return;
    if (!rec_graph_160 && !rec_graph_320 && !rec_graph_640 && !rec_graph_1280) return;

    // Unified two-slot (rec_depth) async pipeline covering both single-bucket
    // lines and long-line sliding-window slices without drain barriers.
    struct GridBoxResult {
        TextBox* box = nullptr;
        std::vector<std::vector<CtcStep>> slices;
        size_t remaining = 0;
    };
    struct PendingRec {
        NpuInferRequest* req = nullptr;
        TextBox* box = nullptr;
        GridBoxResult* grid_box = nullptr;
        size_t slice_idx = 0;
        int cell_start = 0;
        int cell_end = 0;
        int vocab_size = 0;
        int valid_seq_len = 0;
    };

    const int rec_depth = options.rec_depth;
    const int vocab_size = rec_vocab_size;
    std::vector<PendingRec> pending(static_cast<size_t>(rec_depth));
    std::deque<GridBoxResult> grid_results;

    auto consume_rec_slot = [&](size_t slot) {
        PendingRec& p = pending[slot];
        const uint16_t* p_out = reinterpret_cast<const uint16_t*>(p.req->get_output_ptr(0));
        if (p.grid_box != nullptr) {
            auto& dst_slice = p.grid_box->slices[p.slice_idx];
            if (p.cell_end > p.cell_start) {
                dst_slice.reserve(static_cast<size_t>(p.cell_end - p.cell_start));
                for (int t = p.cell_start; t < p.cell_end; ++t) {
                    dst_slice.push_back(CtcDecoder::argmax_fp16(
                        p_out + static_cast<size_t>(t) * static_cast<size_t>(p.vocab_size),
                        p.vocab_size));
                }
            }
            if (--p.grid_box->remaining == 0) {
                p.grid_box->box->text = ctc_decoder->fold_slices(
                    p.grid_box->slices, p.grid_box->box->text_confidence);
                p.grid_box->slices.clear();
            }
        } else {
            p.box->text = ctc_decoder->decode(p_out, p.valid_seq_len, p.vocab_size, p.box->text_confidence);
        }
        p.req = nullptr;
        p.box = nullptr;
        p.grid_box = nullptr;
    };

    auto flush_rec_slot = [&](size_t slot) {
        PendingRec& p = pending[slot];
        if (!p.req) return;
        p.req->wait();
        consume_rec_slot(slot);
    };

    auto acquire_rec_slot = [&](size_t submission_idx) -> size_t {
        if (options.rec_poll && rec_depth == 2) {
            if (pending[0].req && pending[1].req) {
                size_t completed_slot = 0;
                for (;;) {
                    if (pending[0].req->try_wait()) { completed_slot = 0; break; }
                    if (pending[1].req->try_wait()) { completed_slot = 1; break; }
                    Sleep(0);
                }
                consume_rec_slot(completed_slot);
                return completed_slot;
            }
            return pending[0].req ? 1 : 0;
        }
        const size_t slot = submission_idx % static_cast<size_t>(rec_depth);
        flush_rec_slot(slot);
        return slot;
    };

    struct RecBucket {
        int width;
        const std::shared_ptr<NpuGraph>& graph;
    };
    const RecBucket single_buckets[4] = {
        { 160, rec_graph_160 },
        { 320, rec_graph_320 },
        { 640, rec_graph_640 },
        { 1280, rec_graph_1280 },
    };
    const RecBucket sliding_candidates[4] = {
        { 640, rec_graph_640 },
        { 320, rec_graph_320 },
        { 1280, rec_graph_1280 },
        { 160, rec_graph_160 },
    };

    size_t submission_idx = 0;
    for (size_t b = 0; b < boxes.size(); ++b) {
        auto& box = boxes[b];
        // Crop quad normalized to height 48, with natural uncompressed width.
        // Runs before acquire_rec_slot() so cropping overlaps with in-flight NPU work.
        const ImageBGR natural_crop = WicDecoder::crop_quad_natural(image, box.quad, kRecHeight);
        if (natural_crop.empty()) continue;
        const int natural_w = natural_crop.width;

        int target_w = 0;
        std::shared_ptr<NpuGraph> target_graph;
        for (const auto& bucket : single_buckets) {
            if (bucket.graph && natural_w <= bucket.width) {
                target_w = bucket.width;
                target_graph = bucket.graph;
                break;
            }
        }

        if (target_graph) {
            const int valid_w = std::min(target_w, natural_w);
            const size_t slot = acquire_rec_slot(submission_idx++);
            auto& req = target_graph->get_request(slot);
            fill_rec_input(natural_crop, 0, valid_w, target_w, req.get_input_ptr(0));
            req.start_async();

            const int seq_len = target_w / kRecCellStride;
            pending[slot].req = &req;
            pending[slot].box = &box;
            pending[slot].grid_box = nullptr;
            pending[slot].vocab_size = vocab_size;
            pending[slot].valid_seq_len =
                std::min(seq_len, static_cast<int>(std::ceil(static_cast<float>(valid_w) / kRecCellStride)));
        } else {
            int window_w = 0;
            for (const auto& candidate : sliding_candidates) {
                if (candidate.graph) {
                    window_w = candidate.width;
                    target_graph = candidate.graph;
                    break;
                }
            }
            if (!target_graph) continue;

            const int overlap_px = std::min(kLongLineOverlap, window_w / 2);
            const int overlap_cells = overlap_px / kRecCellStride;
            const int half_overlap_cells = overlap_cells / 2;
            const int step_px = window_w - overlap_px;
            const int num_slices = static_cast<int>(
                std::ceil(static_cast<float>(natural_w - overlap_px) / step_px));
            if (num_slices <= 0) continue;

            const int win_cells = window_w / kRecCellStride;
            auto& grid_box = grid_results.emplace_back();
            grid_box.box = &box;
            grid_box.slices.resize(static_cast<size_t>(num_slices));
            grid_box.remaining = static_cast<size_t>(num_slices);

            for (int s = 0; s < num_slices; ++s) {
                const int sx = s * step_px;
                const int valid_slice_w = std::min(window_w, natural_w - sx);
                const int valid_slice_cells = std::min(
                    win_cells,
                    static_cast<int>(std::ceil(static_cast<float>(valid_slice_w) / kRecCellStride)));
                const int cell_start = (s == 0) ? 0 : half_overlap_cells;
                const int cell_end = (s == num_slices - 1) ? valid_slice_cells : (win_cells - half_overlap_cells);

                const size_t slot = acquire_rec_slot(submission_idx++);
                auto& req = target_graph->get_request(slot);
                fill_rec_input(natural_crop, sx, valid_slice_w, window_w, req.get_input_ptr(0));
                req.start_async();

                pending[slot].req = &req;
                pending[slot].box = nullptr;
                pending[slot].grid_box = &grid_box;
                pending[slot].slice_idx = static_cast<size_t>(s);
                pending[slot].cell_start = cell_start;
                pending[slot].cell_end = cell_end;
                pending[slot].vocab_size = vocab_size;
            }
        }
    }

    // Flush all remaining in-flight recognition slots
    for (size_t s = 0; s < pending.size(); ++s) flush_rec_slot(s);
}

OcrEngine::OcrEngine() = default;
OcrEngine::~OcrEngine() = default;

void OcrEngine::initialize(const OcrEngineOptions& options) {
    if (state_ && state_->ready) {
        throw std::runtime_error("OcrEngine::initialize() called on an already initialized engine");
    }

    auto state = std::make_unique<State>();
    state->options = options;

    // Resize mode always fills the 960 bucket, so the tile size only matters
    // when the caller asked for tiles.
    const int det_size = (options.det_mode == DetMode::Resize) ? kDetBucket : options.det_tile_size;
    if (det_size < 32 || det_size > 960 || det_size % 32 != 0) {
        throw std::runtime_error("det_tile_size must be a multiple of 32 from 32 through 960");
    }
    state->options.det_tile_size = det_size;

    if (options.rec_depth < 1 || options.rec_depth > 8) {
        throw std::runtime_error("rec_depth must be from 1 through 8");
    }
    if (options.rec_poll && options.rec_depth != 2) {
        throw std::runtime_error("rec_poll requires rec_depth 2");
    }

    if (options.det_model.empty()) {
        throw std::runtime_error("OcrEngineOptions::det_model is empty");
    }
    const std::wstring det_model = resolve_model(options.det_model);
    if (det_model.empty()) {
        throw std::runtime_error("detection model not found: " + narrow_path(options.det_model));
    }

    const std::wstring dictionary = options.dictionary;
    std::error_code error;
    if (dictionary.empty() || !fs::exists(dictionary, error)) {
        throw std::runtime_error("character dictionary not found: " + narrow_path(dictionary));
    }
    // The recognizer emits one score per dictionary entry plus the CTC blank, so
    // the decoder has to exist before any recognition graph can be validated.
    state->ctc_decoder = std::make_unique<CtcDecoder>(narrow_path(dictionary));
    if (state->ctc_decoder->vocab_size() <= 1) {
        throw std::runtime_error("character dictionary is empty: " + narrow_path(dictionary));
    }

    const std::wstring shared_rec = resolve_model(options.rec_model);
    if (!options.rec_model.empty() && shared_rec.empty()) {
        throw std::runtime_error("recognition model not found: " + narrow_path(options.rec_model));
    }
    const std::wstring requested[4] = {
        options.rec_model_160, options.rec_model_320, options.rec_model_640, options.rec_model_1280 };
    std::wstring rec_paths[4];
    for (int i = 0; i < 4; ++i) {
        if (requested[i].empty()) {
            rec_paths[i] = shared_rec;
            continue;
        }
        rec_paths[i] = resolve_model(requested[i]);
        if (rec_paths[i].empty()) {
            throw std::runtime_error("recognition model not found: " + narrow_path(requested[i]));
        }
    }

    // The NPU runtime is process-wide, so its own state (JIT vs native blobs,
    // cache backend, low-latency queues) has to be armed before any graph load.
    auto& engine = NpuEngine::instance();
    if (!engine.initialize()) {
        throw std::runtime_error("NPU initialization failed");
    }
    engine.set_ngraph_lite(options.use_ngraph_lite);
    engine.set_manual_blob_cache(options.manual_blob_cache);
    engine.set_cache_dir(options.cache_dir.empty() ? std::filesystem::path(L"cache") : std::filesystem::path(options.cache_dir));
    engine.set_turbo(options.turbo);

    const std::array<int64_t, 4> det_fill_dims = options.det_input_u8
        ? std::array<int64_t, 4>{ 1, det_size, det_size, 3 }
        : std::array<int64_t, 4>{ 1, 3, det_size, det_size };
    const std::span<const int64_t> det_fill(det_fill_dims);
    state->det_graph = engine.load_model(
        narrow_path(det_model), 2, det_fill, options.det_sha256); // 2-deep tile pipeline (submit-ahead)

    const std::vector<uint32_t> det_input_dims = options.det_input_u8
        ? std::vector<uint32_t>{ 1, static_cast<uint32_t>(det_size), static_cast<uint32_t>(det_size), 3 }
        : std::vector<uint32_t>{ 1, 3, static_cast<uint32_t>(det_size), static_cast<uint32_t>(det_size) };
    const size_t det_input_bytes = static_cast<size_t>(det_size) * det_size * 3 *
        (options.det_input_u8 ? sizeof(uint8_t) : sizeof(uint16_t));
    const std::vector<uint32_t> det_output_dims = {
        1, 1, static_cast<uint32_t>(det_size), static_cast<uint32_t>(det_size) };
    const size_t det_output_bytes = static_cast<size_t>(det_size) * det_size * sizeof(uint16_t);

    require_single_tensor("detection graph", *state->det_graph);
    require_shape("detection graph", "input", state->det_graph->inputs()[0],
                  det_input_dims, det_input_bytes);
    require_shape("detection graph", "output", state->det_graph->outputs()[0],
                  det_output_dims, det_output_bytes);

    const int bucket_widths[4] = { 160, 320, 640, 1280 };
    state->rec_vocab_size = static_cast<int>(state->ctc_decoder->vocab_size());
    std::shared_ptr<NpuGraph>* const buckets[4] = {
        &state->rec_graph_160, &state->rec_graph_320, &state->rec_graph_640, &state->rec_graph_1280 };
    for (int i = 0; i < 4; ++i) {
        if (rec_paths[i].empty()) continue;

        const int width = bucket_widths[i];
        const int seq_len = width / kRecCellStride;
        const std::array<int64_t, 4> fill_dims = options.rec_input_u8
            ? std::array<int64_t, 4>{ 1, kRecHeight, width, 3 }
            : std::array<int64_t, 4>{ 1, 3, kRecHeight, width };
        const std::span<const int64_t> fill(fill_dims);
        const std::string& rec_sha256 = requested[i].empty() ? options.rec_sha256 : std::string{};
        *buckets[i] = engine.load_model(
            narrow_path(rec_paths[i]), static_cast<size_t>(options.rec_depth), fill, rec_sha256);

        // A graph of the wrong width would decode a different number of cells
        // than the bucket routing assumes, and a graph with the wrong vocabulary
        // would decode the right cells into the wrong characters.
        const std::string graph_name = "rec" + std::to_string(width) + " graph";
        const std::vector<uint32_t> rec_input_dims = options.rec_input_u8
            ? std::vector<uint32_t>{ 1, kRecHeight, static_cast<uint32_t>(width), 3 }
            : std::vector<uint32_t>{ 1, 3, kRecHeight, static_cast<uint32_t>(width) };
        const size_t rec_input_bytes = static_cast<size_t>(kRecHeight) * width * 3 *
            (options.rec_input_u8 ? sizeof(uint8_t) : sizeof(uint16_t));
        const std::vector<uint32_t> rec_output_dims = {
            1, static_cast<uint32_t>(seq_len), static_cast<uint32_t>(state->rec_vocab_size) };
        const size_t rec_output_bytes =
            static_cast<size_t>(seq_len) * state->rec_vocab_size * sizeof(uint16_t);

        require_single_tensor(graph_name, **buckets[i]);
        require_shape(graph_name, "input", (*buckets[i])->inputs()[0], rec_input_dims, rec_input_bytes);
        require_shape(graph_name, "output", (*buckets[i])->outputs()[0], rec_output_dims, rec_output_bytes);
    }

    if (!state->rec_graph_160 && !state->rec_graph_320 && !state->rec_graph_640 && !state->rec_graph_1280) {
        throw std::runtime_error("no recognition model could be loaded");
    }

    state->ready = true;
    state_ = std::move(state);
}

std::vector<OcrLine> OcrEngine::recognize(const ImageBGR& image) {
    if (!state_ || !state_->ready) {
        throw std::runtime_error("OcrEngine::recognize() called before initialize()");
    }
    if (image.empty()) return {};

    ProbHeatmap heatmap(image.width, image.height);
    state_->detect(image, heatmap);

    DbNetPostProcessor post_proc(
        state_->options.det_thresh, state_->options.det_box_thresh, state_->options.det_unclip_ratio);
    std::vector<TextBox> boxes = post_proc.extract_boxes(heatmap, image.width, image.height);

    state_->recognize_boxes(image, boxes);

    // Only lines the recognizer produced no text for are dropped: the CLI's
    // drop_score constant is declared but never applied, and results have to
    // stay identical to it.
    std::vector<OcrLine> lines;
    lines.reserve(boxes.size());
    for (const auto& box : boxes) {
        if (box.text.empty()) continue;
        OcrLine line;
        line.quad = box.quad;
        line.score = box.score;
        line.text = box.text;
        line.confidence = box.text_confidence;
        lines.push_back(std::move(line));
    }
    return lines;
}

bool OcrEngine::initialized() const noexcept {
    return state_ && state_->ready;
}

} // namespace ppocr