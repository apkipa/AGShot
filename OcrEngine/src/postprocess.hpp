#pragma once

#include "wic_decoder.hpp"

#include <cstdint>

#include <string>
#include <vector>

namespace ppocr {

struct TextBox {
    std::vector<Point2f> quad; // 4 vertices: TL, TR, BR, BL
    float score = 0.0f;
    std::string text;
    float text_confidence = 0.0f;
};

// Represents a 2D float probability map with element-wise max stitching support
class ProbHeatmap {
public:
    ProbHeatmap(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<float>& data() const { return data_; }
    std::vector<float>& data() { return data_; }

    // Merges a sub-tile probability map using element-wise max (no feathering decay)
    void stitch_tile_max(
        const float* tile_probs,
        int tile_w, int tile_h,
        int offset_x, int offset_y
    );

    // Fast path for detector output: decode FP16 directly while stitching,
    // avoiding a full per-tile float allocation and copy.
    void stitch_tile_max_fp16(
        const uint16_t* tile_probs,
        int tile_w, int tile_h,
        int offset_x, int offset_y
    );

private:
    int width_;
    int height_;
    std::vector<float> data_;
};

class DbNetPostProcessor {
public:
    explicit DbNetPostProcessor(float thresh = 0.2f, float box_thresh = 0.45f, float unclip_ratio = 1.4f);

    // Official-grade DBNet contour extraction, Clipper unclip, and Rotating Calipers
    std::vector<TextBox> extract_boxes(const ProbHeatmap& heatmap, int orig_w, int orig_h);

private:
    float thresh_;
    float box_thresh_;
    float unclip_ratio_;
};

struct CtcStep {
    int index = 0;
    float confidence = 0.0f;
};

class CtcDecoder {
public:
    explicit CtcDecoder(const std::string& dict_path);

    static CtcStep argmax_fp16(const uint16_t* step_slice, int vocab_size) noexcept;

    // Greedy decode from unnormalized Logits or Softmax probabilities
    std::string decode(const uint16_t* data_fp16, int seq_len, int vocab_size, float& out_conf) const;

    // Fold pre-decoded per-step argmax tokens across contiguous slices.
    std::string fold_slices(const std::vector<std::vector<CtcStep>>& slices, float& out_conf) const;

    size_t vocab_size() const { return dict_.size() + 1; } // +1 for blank at index 0

private:
    void append_ctc_step(const CtcStep& step, int& prev_idx, std::string& text,
                         float& conf_sum, int& char_count) const;

    std::vector<std::string> dict_;
};

} // namespace ppocr