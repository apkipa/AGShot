#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ppocr {

struct ImageBGR {
    int width = 0;
    int height = 0;
    int channels = 3;
    std::vector<uint8_t> data; // Continuous 24bpp BGR

    bool empty() const { return data.empty() || width <= 0 || height <= 0; }
};

struct Point2f {
    float x = 0.0f;
    float y = 0.0f;
};

class WicDecoder {
public:
    // Decodes an image from file (JPG, PNG, BMP, TIFF, WebP) into 24bpp BGR via Windows WIC
    static bool decode_file(const std::wstring& file_path, ImageBGR& out_image);

    // Decodes an image from Win32 Clipboard (CF_DIB / CF_DIBV5) into 24bpp BGR
    static bool decode_clipboard(ImageBGR& out_image);

    // Extracts a sub-rectangle tile with zero padding if out of bounds (for slicing)
    static ImageBGR extract_tile(const ImageBGR& src, int x, int y, int w, int h, uint8_t pad_val = 0);

    // Resizes a BGR image with the same 8-bit fixed-point INTER_LINEAR path
    // PaddleOCR uses (via OpenCV), matching the reference detector preprocessing.
    static ImageBGR resize_linear(const ImageBGR& src, int dst_w, int dst_h);

    // Crops a quad (4 points) from source image and perspective/affine warps to dst_w x dst_h BGR
    // Supports physical aspect-ratio preservation with zero-padding
    static ImageBGR crop_quad(const ImageBGR& src, const std::vector<Point2f>& quad, int dst_w, int dst_h);

    // Crops an arbitrary-length quad normalized to height 48, with natural uncompressed width
    static ImageBGR crop_quad_natural(const ImageBGR& src, const std::vector<Point2f>& quad, int target_h = 48);

    // Converts uint8 BGR [H, W, 3] to normalized planar NCHW FP16 buffer
    static void normalize_to_nchw_fp16(
        const ImageBGR& src,
        void* dst_fp16,
        float mean_b = 0.485f * 255.0f,
        float mean_g = 0.456f * 255.0f,
        float mean_r = 0.406f * 255.0f,
        float std_b = 0.229f * 255.0f,
        float std_g = 0.224f * 255.0f,
        float std_r = 0.225f * 255.0f
    );
};

// Fast IEEE 754 FP32 -> FP16 converter
inline uint16_t float_to_fp16(float val) {
    uint32_t x = *reinterpret_cast<uint32_t*>(&val);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        return static_cast<uint16_t>(sign);
    } else if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00);
    }
    return static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
}

inline float fp16_to_float(uint16_t val) {
    uint32_t sign = (val & 0x8000) << 16;
    int32_t exp = (val >> 10) & 0x1f;
    uint32_t mant = val & 0x3ff;
    if (exp == 0) {
        return 0.0f;
    } else if (exp == 31) {
        uint32_t res = sign | 0x7f800000 | (mant << 13);
        return *reinterpret_cast<float*>(&res);
    }
    uint32_t res = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    return *reinterpret_cast<float*>(&res);
}

} // namespace ppocr
