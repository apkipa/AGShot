#pragma once

#include "wic_decoder.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ppocr {

// One recognized text line.
struct OcrLine {
    std::vector<Point2f> quad; // TL, TR, BR, BL in source-image pixels
    float score = 0.0f;        // detection confidence
    std::string text;          // recognized UTF-8 text
    float confidence = 0.0f;   // recognition confidence
};

// How detection covers an image that does not fit the detector's bucket.
enum class DetMode {
    Resize, // scale the long edge to fit, letterbox into the bucket
    Tile,   // overlapping tiles, probability maps merged by element-wise max
};

struct OcrEngineOptions {
    // Model paths. A path may name a ready .ngraph_lite file or an
    // extension-less base, in which case .ngraph_lite is appended. The graph
    // loader itself still takes narrow paths, so a model outside the active
    // code page cannot be opened.
    std::wstring det_model;
    std::wstring rec_model;     // shape template shared by every recognition bucket
    std::wstring rec_model_160; // per-bucket overrides; empty falls back to rec_model
    std::wstring rec_model_320;
    std::wstring rec_model_640;
    std::wstring rec_model_1280;
    std::wstring dictionary;    // ppocr_keys_v1.txt

    DetMode det_mode = DetMode::Resize;
    int det_tile_size = 960; // tile edge for DetMode::Tile, a multiple of 32 up to 960
    float det_thresh = 0.2f; // DBNet probability threshold
    float det_box_thresh = 0.45f;
    float det_unclip_ratio = 1.4f;

    // Recognition crops are 48 pixels high and routed to the narrowest bucket
    // that holds their natural width; wider lines fall back to a sliding window
    // over the 640 bucket.
    int rec_depth = 2;     // in-flight requests per recognition graph
    bool rec_poll = false; // reuse whichever slot finished first (needs depth 2)

    // Model-side preprocessing. Off means the host normalizes FP16 NCHW with
    // ImageNet statistics for det and 127.5 for rec.
    bool det_input_u8 = true;
    bool rec_input_u8 = true;

    bool use_ngraph_lite = true;   // load the .ngraph_lite container through its JIT path
    bool manual_blob_cache = true; // keep compiled blobs in the app's own cache
    std::wstring cache_dir;        // AGShot-owned cache root; empty keeps the CLI default
    bool turbo = false;            // low-latency command queues (higher power)
    std::string det_sha256;        // expected lowercase hex SHA-256 of det container; empty skips check
    std::string rec_sha256;        // expected lowercase hex SHA-256 of shared rec container; empty skips check
};

// Synchronous det -> crop -> rec pipeline over the NPU.
//
// The NPU itself is a process-wide singleton, so one engine per process is the
// intended use. initialize() loads and validates every graph and throws
// std::runtime_error if it cannot; recognize() then blocks until its result is
// ready, which makes it safe to call straight from a UI thread as long as the
// caller accepts the wait. NPU queues, USM buffers and request pooling stay
// inside this class.
class OcrEngine {
public:
    OcrEngine();
    ~OcrEngine();
    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    // Loads the models. Throws std::runtime_error on failure, including when it
    // is called on an already initialized engine: destroy and rebuild the
    // engine to switch models.
    void initialize(const OcrEngineOptions& options);

    // Runs det -> crop -> rec over a BGR image and returns the recognized lines
    // in reading order. May be called any number of times; the models stay
    // resident between calls.
    //
    // The pipeline has no text-line orientation classifier, so an upside-down
    // line decodes right-to-left into garbage: the quad reordering in the crop
    // only handles vertical text geometry, it is not a 0/180 decision.
    std::vector<OcrLine> recognize(const ImageBGR& image);

    bool initialized() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ppocr
