#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wic_decoder.hpp"

#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace ppocr {

namespace {

class ComScope {
public:
    ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};

// CvRound: round half to even, matching OpenCV's coefficient rounding.
short cv_round_to_short(double value) {
    return static_cast<short>(std::nearbyint(value));
}

} // namespace

bool WicDecoder::decode_file(const std::wstring& file_path, ImageBGR& out_image) {
    ComScope com;
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pFactory)
    );
    if (FAILED(hr) || !pFactory) {
        std::cerr << "[-] Failed to create WIC Imaging Factory: 0x" << std::hex << hr << "\n";
        return false;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromFilename(
        file_path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &pDecoder
    );
    if (FAILED(hr) || !pDecoder) {
        pFactory->Release();
        std::wcerr << L"[-] WIC failed to decode file: " << file_path << L"\n";
        return false;
    }

    IWICBitmapFrameDecode* pFrame = nullptr;
    pDecoder->GetFrame(0, &pFrame);

    IWICFormatConverter* pConverter = nullptr;
    pFactory->CreateFormatConverter(&pConverter);

    // Lock strictly to 24bpp BGR matching PaddleOCR pretrained space
    pConverter->Initialize(
        pFrame,
        GUID_WICPixelFormat24bppBGR,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom
    );

    UINT width = 0, height = 0;
    pConverter->GetSize(&width, &height);

    out_image.width = static_cast<int>(width);
    out_image.height = static_cast<int>(height);
    out_image.channels = 3;
    UINT stride = width * 3;
    out_image.data.resize(stride * height);

    hr = pConverter->CopyPixels(nullptr, stride, static_cast<UINT>(out_image.data.size()), out_image.data.data());

    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();

    return SUCCEEDED(hr);
}

bool WicDecoder::decode_clipboard(ImageBGR& out_image) {
    if (!OpenClipboard(nullptr)) {
        std::cerr << "[-] Cannot open Windows Clipboard.\n";
        return false;
    }

    HANDLE hData = GetClipboardData(CF_DIB);
    if (!hData) {
        CloseClipboard();
        std::cerr << "[-] No image (CF_DIB) found in Windows Clipboard.\n";
        return false;
    }

    void* pData = GlobalLock(hData);
    if (!pData) {
        CloseClipboard();
        return false;
    }

    const BITMAPINFOHEADER* bih = reinterpret_cast<const BITMAPINFOHEADER*>(pData);
    int width = bih->biWidth;
    int height = bih->biHeight;
    bool bottom_up = (height > 0);
    int abs_height = std::abs(height);
    int bit_count = bih->biBitCount;

    if (bit_count != 24 && bit_count != 32) {
        GlobalUnlock(hData);
        CloseClipboard();
        std::cerr << "[-] Clipboard bit depth " << bit_count << " not directly supported (expected 24 or 32 bpp).\n";
        return false;
    }

    const uint8_t* pixels = reinterpret_cast<const uint8_t*>(pData) + bih->biSize;
    if (bih->biCompression == BI_BITFIELDS) {
        pixels += 12; // skip masks
    }

    out_image.width = width;
    out_image.height = abs_height;
    out_image.channels = 3;
    out_image.data.resize(width * abs_height * 3);

    int src_stride = ((width * bit_count + 31) / 32) * 4;
    int dst_stride = width * 3;

    for (int y = 0; y < abs_height; ++y) {
        int src_y = bottom_up ? (abs_height - 1 - y) : y;
        const uint8_t* src_row = pixels + src_y * src_stride;
        uint8_t* dst_row = out_image.data.data() + y * dst_stride;

        if (bit_count == 24) {
            std::memcpy(dst_row, src_row, dst_stride);
        } else if (bit_count == 32) {
            // Drop Alpha channel: BGRA -> BGR
            for (int x = 0; x < width; ++x) {
                dst_row[x * 3 + 0] = src_row[x * 4 + 0];
                dst_row[x * 3 + 1] = src_row[x * 4 + 1];
                dst_row[x * 3 + 2] = src_row[x * 4 + 2];
            }
        }
    }

    GlobalUnlock(hData);
    CloseClipboard();
    return true;
}

ImageBGR WicDecoder::extract_tile(const ImageBGR& src, int x, int y, int w, int h, uint8_t pad_val) {
    ImageBGR tile;
    tile.width = w;
    tile.height = h;
    tile.channels = 3;
    tile.data.assign(w * h * 3, pad_val);

    int src_x_start = std::max(0, x);
    int src_y_start = std::max(0, y);
    int src_x_end = std::min(src.width, x + w);
    int src_y_end = std::min(src.height, y + h);

    for (int cy = src_y_start; cy < src_y_end; ++cy) {
        int dst_y = cy - y;
        int dst_x = src_x_start - x;
        int copy_bytes = (src_x_end - src_x_start) * 3;

        const uint8_t* p_src = src.data.data() + (cy * src.width + src_x_start) * 3;
        uint8_t* p_dst = tile.data.data() + (dst_y * w + dst_x) * 3;
        std::memcpy(p_dst, p_src, copy_bytes);
    }

    return tile;
}

ImageBGR WicDecoder::resize_linear(const ImageBGR& src, int dst_w, int dst_h) {
    ImageBGR dst;
    if (src.empty() || dst_w <= 0 || dst_h <= 0) return dst;

    dst.width = dst_w;
    dst.height = dst_h;
    dst.channels = 3;
    dst.data.resize(static_cast<size_t>(dst_w) * dst_h * 3);

    const int scale = 1 << 11;

    // Horizontal taps: same as PPOCRPreprocess.BuildLinearCoefficients
    std::vector<int> x_offsets(dst_w);
    std::vector<short> x_coeffs(static_cast<size_t>(dst_w) * 2);
    for (int d = 0; d < dst_w; ++d) {
        double coordinate = (static_cast<double>(d) + 0.5) * src.width / dst_w - 0.5;
        int source = static_cast<int>(std::floor(coordinate));
        double fraction = coordinate - source;
        if (source < 0) { source = 0; fraction = 0.0; }
        if (source >= src.width - 1) { source = src.width - 1; fraction = 0.0; }
        x_offsets[d] = source;
        x_coeffs[d * 2] = cv_round_to_short((1.0 - fraction) * scale);
        x_coeffs[d * 2 + 1] = cv_round_to_short(fraction * scale);
    }

    // Vertical taps: edges keep the raw fraction (OpenCV clips source rows only)
    std::vector<int> y_source(dst_h);
    std::vector<short> y_coeffs(static_cast<size_t>(dst_h) * 2);
    for (int d = 0; d < dst_h; ++d) {
        double coordinate = (static_cast<double>(d) + 0.5) * src.height / dst_h - 0.5;
        int source = static_cast<int>(std::floor(coordinate));
        double fraction = coordinate - source;
        y_source[d] = source;
        y_coeffs[d * 2] = cv_round_to_short((1.0 - fraction) * scale);
        y_coeffs[d * 2 + 1] = cv_round_to_short(fraction * scale);
    }

    std::vector<int> row0(static_cast<size_t>(dst_w) * 3);
    std::vector<int> row1(static_cast<size_t>(dst_w) * 3);

    for (int oy = 0; oy < dst_h; ++oy) {
        int sy = y_source[oy];
        int sy0 = std::clamp(sy, 0, src.height - 1);
        int sy1 = std::clamp(sy + 1, 0, src.height - 1);
        const uint8_t* row0_src = src.data.data() + static_cast<size_t>(sy0) * src.width * 3;
        const uint8_t* row1_src = src.data.data() + static_cast<size_t>(sy1) * src.width * 3;

        for (int ox = 0; ox < dst_w; ++ox) {
            int sx = x_offsets[ox];
            int sx1 = std::min(sx + 1, src.width - 1);
            short c0 = x_coeffs[ox * 2];
            short c1 = x_coeffs[ox * 2 + 1];
            int so = sx * 3, so1 = sx1 * 3, doff = ox * 3;
            for (int ch = 0; ch < 3; ++ch) {
                row0[doff + ch] = row0_src[so + ch] * c0 + row0_src[so1 + ch] * c1;
                row1[doff + ch] = row1_src[so + ch] * c0 + row1_src[so1 + ch] * c1;
            }
        }

        short b0 = y_coeffs[oy * 2];
        short b1 = y_coeffs[oy * 2 + 1];
        uint8_t* dst_row = dst.data.data() + static_cast<size_t>(oy) * dst_w * 3;
        for (int ox = 0; ox < dst_w; ++ox) {
            int doff = ox * 3;
            for (int ch = 0; ch < 3; ++ch) {
                int h0 = row0[doff + ch];
                int h1 = row1[doff + ch];
                int value = (((h0 >> 4) * b0 >> 16) + ((h1 >> 4) * b1 >> 16) + 2) >> 2;
                if (value < 0) value = 0;
                else if (value > 255) value = 255;
                dst_row[doff + ch] = static_cast<uint8_t>(value);
            }
        }
    }

    return dst;
}

ImageBGR WicDecoder::crop_quad_natural(const ImageBGR& src, const std::vector<Point2f>& quad, int target_h) {
    ImageBGR dst;
    if (quad.size() < 4) return dst;

    Point2f p0 = quad[0], p1 = quad[1], p2 = quad[2], p3 = quad[3];

    // Official PaddleOCR get_rotate_crop_image logic:
    // 1. Calculate width & height of the quadrilateral
    float box_w = std::max(std::hypot(p1.x - p0.x, p1.y - p0.y), std::hypot(p2.x - p3.x, p2.y - p3.y));
    float box_h = std::max(std::hypot(p3.x - p0.x, p3.y - p0.y), std::hypot(p2.x - p1.x, p2.y - p1.y));
    if (box_w < 1.0f || box_h < 1.0f) return dst;

    bool is_vertical = (box_h >= 1.5f * box_w); // Vertical text auto-rotation

    float natural_aspect = is_vertical ? (box_h / box_w) : (box_w / box_h);
    int natural_w = std::max(16, static_cast<int>(std::round(target_h * natural_aspect)));

    dst.width = natural_w;
    dst.height = target_h;
    dst.channels = 3;
    dst.data.assign(natural_w * target_h * 3, 0);

    // If vertical text: rotate 90 degrees CCW
    if (is_vertical) {
        // Swap vertices: p0->p1, p1->p2, p2->p3, p3->p0 to rotate orientation
        Point2f rot_p0 = p1, rot_p1 = p2, rot_p2 = p3, rot_p3 = p0;
        p0 = rot_p0; p1 = rot_p1; p2 = rot_p2; p3 = rot_p3;
    }

    for (int y = 0; y < target_h; ++y) {
        float v = (target_h > 1) ? static_cast<float>(y) / (target_h - 1) : 0.0f;
        Point2f left = { p0.x + v * (p3.x - p0.x), p0.y + v * (p3.y - p0.y) };
        Point2f right = { p1.x + v * (p2.x - p1.x), p1.y + v * (p2.y - p1.y) };

        for (int x = 0; x < natural_w; ++x) {
            float u = (natural_w > 1) ? static_cast<float>(x) / (natural_w - 1) : 0.0f;
            float src_x = left.x + u * (right.x - left.x);
            float src_y = left.y + u * (right.y - left.y);

            int x0 = static_cast<int>(std::floor(src_x));
            int y0 = static_cast<int>(std::floor(src_y));
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            if (x0 >= 0 && x1 < src.width && y0 >= 0 && y1 < src.height) {
                float fx = src_x - x0;
                float fy = src_y - y0;
                float w00 = (1.0f - fx) * (1.0f - fy);
                float w10 = fx * (1.0f - fy);
                float w01 = (1.0f - fx) * fy;
                float w11 = fx * fy;

                const uint8_t* p00 = src.data.data() + (y0 * src.width + x0) * 3;
                const uint8_t* p10 = src.data.data() + (y0 * src.width + x1) * 3;
                const uint8_t* p01 = src.data.data() + (y1 * src.width + x0) * 3;
                const uint8_t* p11 = src.data.data() + (y1 * src.width + x1) * 3;

                uint8_t* p_out = dst.data.data() + (y * natural_w + x) * 3;
                for (int c = 0; c < 3; ++c) {
                    float val = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
                    p_out[c] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
                }
            }
        }
    }
    return dst;
}

void WicDecoder::normalize_to_nchw_fp16(
    const ImageBGR& src,
    void* dst_fp16,
    float mean_b, float mean_g, float mean_r,
    float std_b, float std_g, float std_r
) {
    uint16_t* p_dst = reinterpret_cast<uint16_t*>(dst_fp16);
    int hw = src.width * src.height;
    uint16_t* p_b = p_dst;
    uint16_t* p_g = p_dst + hw;
    uint16_t* p_r = p_dst + hw * 2;

    const uint8_t* p_src = src.data.data();
    for (int i = 0; i < hw; ++i) {
        float b = (static_cast<float>(p_src[i * 3 + 0]) - mean_b) / std_b;
        float g = (static_cast<float>(p_src[i * 3 + 1]) - mean_g) / std_g;
        float r = (static_cast<float>(p_src[i * 3 + 2]) - mean_r) / std_r;

        p_b[i] = float_to_fp16(b);
        p_g[i] = float_to_fp16(g);
        p_r[i] = float_to_fp16(r);
    }
}

} // namespace ppocr
