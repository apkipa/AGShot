#include "postprocess.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>

namespace ppocr {

// ============================================================================
// ProbHeatmap Implementation (Element-wise Max Stitching)
// ============================================================================

ProbHeatmap::ProbHeatmap(int width, int height)
    : width_(width), height_(height), data_(width * height, 0.0f) {}

void ProbHeatmap::stitch_tile_max(
    const float* tile_probs,
    int tile_w, int tile_h,
    int offset_x, int offset_y
) {
    const int x0 = std::max(0, -offset_x);
    const int x1 = std::min(tile_w, width_ - offset_x);
    const int y0 = std::max(0, -offset_y);
    const int y1 = std::min(tile_h, height_ - offset_y);
    for (int ty = y0; ty < y1; ++ty) {
        const float* src = tile_probs + static_cast<size_t>(ty) * tile_w + x0;
        float* dst = data_.data() + static_cast<size_t>(offset_y + ty) * width_ + offset_x + x0;
        for (int tx = x0; tx < x1; ++tx, ++src, ++dst) {
            if (*src > *dst) *dst = *src;
        }
    }
}

void ProbHeatmap::stitch_tile_max_fp16(
    const uint16_t* tile_probs,
    int tile_w, int tile_h,
    int offset_x, int offset_y
) {
    const int x0 = std::max(0, -offset_x);
    const int x1 = std::min(tile_w, width_ - offset_x);
    const int y0 = std::max(0, -offset_y);
    const int y1 = std::min(tile_h, height_ - offset_y);
    for (int ty = y0; ty < y1; ++ty) {
        const uint16_t* src = tile_probs + static_cast<size_t>(ty) * tile_w + x0;
        float* dst = data_.data() + static_cast<size_t>(offset_y + ty) * width_ + offset_x + x0;
        for (int tx = x0; tx < x1; ++tx, ++src, ++dst) {
            const float value = fp16_to_float(*src);
            if (value > *dst) *dst = value;
        }
    }
}

// ============================================================================
// Geometry & Suzuki Contour Tracing (Zero OpenCV Dependency)
// ============================================================================

namespace {

struct Point2i {
    int x, y;
};

// 8-neighbor offsets for Suzuki contour tracing
const int DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
const int DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

// Standalone Suzuki 8-connectivity outer contour tracer
std::vector<std::vector<Point2i>> find_contours_binary(const std::vector<uint8_t>& binary, int w, int h) {
    std::vector<std::vector<Point2i>> contours;
    std::vector<int> image(binary.begin(), binary.end()); // mutable label buffer

    int nbd = 1;

    for (int y = 0; y < h; ++y) {
        int ln = 1;
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            int f = image[idx];

            bool is_outer = (f == 1 && (x == 0 || image[idx - 1] == 0));
            if (is_outer) {
                nbd++;
                int x0 = x, y0 = y;
                // The border was entered from the west: the scan reached this
                // pixel with its left neighbour clear, so the search starts at
                // direction 4 (dx=-1, dy=0).
                int dir = 4;

                // Find initial clockwise neighbor
                int start_dir = -1;
                for (int d = 0; d < 8; ++d) {
                    int cdir = (dir + d) % 8;
                    int nx = x0 + DX[cdir];
                    int ny = y0 + DY[cdir];
                    if (nx >= 0 && nx < w && ny >= 0 && ny < h && image[ny * w + nx] != 0) {
                        start_dir = cdir;
                        break;
                    }
                }

                if (start_dir == -1) {
                    // Isolated single pixel
                    image[idx] = -nbd;
                    contours.push_back({ {x0, y0} });
                } else {
                    std::vector<Point2i> contour;
                    contour.push_back({ x0, y0 });

                    int cx = x0 + DX[start_dir];
                    int cy = y0 + DY[start_dir];
                    contour.push_back({ cx, cy });

                    int prev_dir = (start_dir + 4) % 8;
                    int curr_x = cx, curr_y = cy;

                    while (true) {
                        int next_dir = -1;
                        for (int d = 1; d <= 8; ++d) {
                            int cdir = (prev_dir + d) % 8;
                            int nx = curr_x + DX[cdir];
                            int ny = curr_y + DY[cdir];
                            if (nx >= 0 && nx < w && ny >= 0 && ny < h && image[ny * w + nx] != 0) {
                                next_dir = cdir;
                                break;
                            }
                        }

                        if (next_dir == -1) break;

                        int nx = curr_x + DX[next_dir];
                        int ny = curr_y + DY[next_dir];

                        if (curr_x == x0 && curr_y == y0 && nx == x0 + DX[start_dir] && ny == y0 + DY[start_dir]) {
                            break; // Loop completed
                        }

                        image[curr_y * w + curr_x] = nbd;
                        contour.push_back({ nx, ny });
                        prev_dir = (next_dir + 4) % 8;
                        curr_x = nx;
                        curr_y = ny;

                        if (contour.size() > 20000) break; // Safeguard
                    }
                    if (contour.size() >= 4) {
                        // Simplify in place: drop consecutive duplicates and collinear
                        // middles so the hull / rotating-calipers pass sees ~7x fewer
                        // points (equivalent to cv2 CHAIN_APPROX_SIMPLE output).
                        size_t out = 0;
                        for (size_t i = 0; i < contour.size(); ++i) {
                            const Point2i p = contour[i];
                            while (out >= 2) {
                                const Point2i& a = contour[out - 2];
                                const Point2i& b = contour[out - 1];
                                long long cross =
                                    static_cast<long long>(b.x - a.x) * (p.y - a.y) -
                                    static_cast<long long>(b.y - a.y) * (p.x - a.x);
                                if (cross != 0) break;
                                --out;
                            }
                            if (out >= 1 && contour[out - 1].x == p.x && contour[out - 1].y == p.y) {
                                continue;
                            }
                            contour[out++] = p;
                        }
                        contour.resize(out);
                        if (contour.size() >= 4) {
                            contours.push_back(contour);
                        }
                    }
                }
            }
            if (f != 0 && f != 1) ln = std::abs(f);
        }
    }
    return contours;
}

// 2D Cross product of (b - a) x (c - a)
float cross_product(const Point2f& a, const Point2f& b, const Point2f& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Computes 2D Convex Hull (Monotone Chain algorithm)
std::vector<Point2f> convex_hull(std::vector<Point2f> pts) {
    size_t n = pts.size();
    if (n <= 2) return pts;

    std::sort(pts.begin(), pts.end(), [](const Point2f& a, const Point2f& b) {
        return (a.x < b.x) || (a.x == b.x && a.y < b.y);
    });

    std::vector<Point2f> h;
    for (size_t i = 0; i < n; ++i) {
        while (h.size() >= 2 && cross_product(h[h.size() - 2], h[h.size() - 1], pts[i]) <= 0) {
            h.pop_back();
        }
        h.push_back(pts[i]);
    }

    size_t lower_sz = h.size() + 1;
    for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
        while (h.size() >= lower_sz && cross_product(h[h.size() - 2], h[h.size() - 1], pts[i]) <= 0) {
            h.pop_back();
        }
        h.push_back(pts[i]);
    }
    h.pop_back();
    return h;
}

// Rotating Calipers: calculates minimum area rotated bounding box (exact OpenCV minAreaRect equivalent)
std::vector<Point2f> min_area_rect(const std::vector<Point2f>& pts) {
    std::vector<Point2f> hull = convex_hull(pts);
    if (hull.size() < 3) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto& p : pts) {
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        return { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} };
    }

    size_t m = hull.size();
    float min_area = 1e18f;
    std::vector<Point2f> best_quad(4);

    for (size_t i = 0; i < m; ++i) {
        Point2f p1 = hull[i];
        Point2f p2 = hull[(i + 1) % m];

        float edge_x = p2.x - p1.x;
        float edge_y = p2.y - p1.y;
        float len = std::hypot(edge_x, edge_y);
        if (len < 1e-4f) continue;

        float cos_theta = edge_x / len;
        float sin_theta = edge_y / len;

        float u_min = 1e9f, u_max = -1e9f;
        float v_min = 1e9f, v_max = -1e9f;

        for (size_t j = 0; j < m; ++j) {
            float u = hull[j].x * cos_theta + hull[j].y * sin_theta;
            float v = -hull[j].x * sin_theta + hull[j].y * cos_theta;
            u_min = std::min(u_min, u); u_max = std::max(u_max, u);
            v_min = std::min(v_min, v); v_max = std::max(v_max, v);
        }

        float area = (u_max - u_min) * (v_max - v_min);
        if (area < min_area) {
            min_area = area;
            // 4 corners in rotated space
            Point2f c[4] = {
                { u_min, v_min },
                { u_max, v_min },
                { u_max, v_max },
                { u_min, v_max }
            };
            for (int k = 0; k < 4; ++k) {
                best_quad[k].x = c[k].x * cos_theta - c[k].y * sin_theta;
                best_quad[k].y = c[k].x * sin_theta + c[k].y * cos_theta;
            }
        }
    }

    // Sort corners: TL, TR, BR, BL
    std::sort(best_quad.begin(), best_quad.end(), [](const Point2f& a, const Point2f& b) {
        return (a.x < b.x) || (a.x == b.x && a.y < b.y);
    });

    Point2f left1 = best_quad[0], left2 = best_quad[1];
    Point2f right1 = best_quad[2], right2 = best_quad[3];

    Point2f tl = (left1.y < left2.y) ? left1 : left2;
    Point2f bl = (left1.y < left2.y) ? left2 : left1;
    Point2f tr = (right1.y < right2.y) ? right1 : right2;
    Point2f br = (right1.y < right2.y) ? right2 : right1;

    return { tl, tr, br, bl };
}

// Fast convex-quad interior score via per-row scanline intervals.
// For a convex polygon the horizontal line at py = y + 0.5 crosses exactly one
// interval [x_min, x_max], so the expensive per-pixel 4-cross test collapses to
// four edge intersections per row plus a contiguous row sum.
float box_score_fast(const ProbHeatmap& heatmap, const std::vector<Point2f>& quad) {
    int w = heatmap.width();
    int h = heatmap.height();

    float y0f = quad[0].y, y1f = quad[0].y;
    for (int i = 1; i < 4; ++i) {
        y0f = std::min(y0f, quad[i].y);
        y1f = std::max(y1f, quad[i].y);
    }
    int iy0 = std::clamp(static_cast<int>(std::floor(y0f)), 0, h - 1);
    int iy1 = std::clamp(static_cast<int>(std::ceil(y1f)), 0, h - 1);

    const auto& probs = heatmap.data();
    double score_sum = 0.0;
    long pixel_count = 0;

    for (int y = iy0; y <= iy1; ++y) {
        float py = static_cast<float>(y) + 0.5f;
        float x_min = 1e9f;
        float x_max = -1e9f;
        int hits = 0;
        for (int i = 0; i < 4; ++i) {
            const Point2f& a = quad[i];
            const Point2f& b = quad[(i + 1) & 3];
            if ((py >= a.y && py <= b.y) || (py >= b.y && py <= a.y)) {
                if (a.y == b.y) {
                    x_min = std::min(x_min, std::min(a.x, b.x));
                    x_max = std::max(x_max, std::max(a.x, b.x));
                    hits += 2;
                } else {
                    float x = a.x + (py - a.y) * (b.x - a.x) / (b.y - a.y);
                    x_min = std::min(x_min, x);
                    x_max = std::max(x_max, x);
                    hits += 1;
                }
            }
        }
        if (hits < 2) continue;

        int xl = std::clamp(static_cast<int>(std::ceil(x_min - 0.5f)), 0, w - 1);
        int xr = std::clamp(static_cast<int>(std::floor(x_max - 0.5f)), 0, w - 1);
        if (xr < xl) continue;

        const float* row = probs.data() + static_cast<size_t>(y) * w;
        for (int x = xl; x <= xr; ++x) {
            score_sum += row[x];
        }
        pixel_count += (xr - xl + 1);
    }
    return (pixel_count > 0) ? static_cast<float>(score_sum / static_cast<double>(pixel_count)) : 0.0f;
}

} // namespace

// ============================================================================
// DbNetPostProcessor Implementation
// ============================================================================

DbNetPostProcessor::DbNetPostProcessor(float thresh, float box_thresh, float unclip_ratio)
    : thresh_(thresh), box_thresh_(box_thresh), unclip_ratio_(unclip_ratio) {}

std::vector<TextBox> DbNetPostProcessor::extract_boxes(const ProbHeatmap& heatmap, int orig_w, int orig_h) {
    int w = heatmap.width();
    int h = heatmap.height();
    int total_pixels = w * h;

    const auto& raw_probs = heatmap.data();
    std::vector<uint8_t> binary_mask(total_pixels, 0);

    for (int i = 0; i < total_pixels; ++i) {
        if (raw_probs[i] >= thresh_) {
            binary_mask[i] = 1;
        }
    }

    // 1. Horizontal Morphological Dilation (3x2 Kernel) to bridge code blocks & space gaps
    std::vector<uint8_t> dilated(total_pixels, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (binary_mask[y * w + x]) {
                for (int dy = 0; dy <= 1 && y + dy < h; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx;
                        if (nx >= 0 && nx < w) {
                            dilated[(y + dy) * w + nx] = 1;
                        }
                    }
                }
            }
        }
    }

    // 2. Suzuki 8-connectivity Contour Extraction
    std::vector<std::vector<Point2i>> contours = find_contours_binary(dilated, w, h);

    std::vector<TextBox> boxes;
    float scale_x = static_cast<float>(orig_w) / w;
    float scale_y = static_cast<float>(orig_h) / h;

    for (const auto& cnt : contours) {
        if (cnt.size() < 4) continue;

        std::vector<Point2f> poly;
        for (const auto& p : cnt) {
            poly.push_back({ static_cast<float>(p.x), static_cast<float>(p.y) });
        }

        // 3. Official PaddleOCR order: fit the tight rotated box and score it while it
        // still hugs the text before the unclip expands it into background.
        std::vector<Point2f> quad = min_area_rect(poly);

        float score = box_score_fast(heatmap, quad);
        if (score < box_thresh_) continue;

        float tight_w = std::hypot(quad[1].x - quad[0].x, quad[1].y - quad[0].y);
        float tight_h = std::hypot(quad[3].x - quad[0].x, quad[3].y - quad[0].y);
        if (tight_w < 3.0f || tight_h < 3.0f) continue;

        // 4. Analytic unclip: the tight box is a rectangle, so a round-join offset is
        // exactly the same rectangle grown by d on every side:
        //   d = area * unclip_ratio / perimeter = w*h*ratio / (2*(w+h))
        // This replaces ClipperLib offsetting plus a second rotating-calipers pass.
        // Verified equal to the Clipper result within sub-pixel rounding and within
        // 1px of the official pyclipper path (which quantizes to whole pixels).
        float d = tight_w * tight_h * unclip_ratio_ / (2.0f * (tight_w + tight_h));
        float half_w = tight_w * 0.5f + d;
        float half_h = tight_h * 0.5f + d;
        Point2f center{ (quad[0].x + quad[2].x) * 0.5f, (quad[0].y + quad[2].y) * 0.5f };
        Point2f u{ (quad[1].x - quad[0].x) / tight_w, (quad[1].y - quad[0].y) / tight_w };
        Point2f v{ (quad[3].x - quad[0].x) / tight_h, (quad[3].y - quad[0].y) / tight_h };
        quad[0] = { center.x - u.x * half_w - v.x * half_h, center.y - u.y * half_w - v.y * half_h };
        quad[1] = { center.x + u.x * half_w - v.x * half_h, center.y + u.y * half_w - v.y * half_h };
        quad[2] = { center.x + u.x * half_w + v.x * half_h, center.y + u.y * half_w + v.y * half_h };
        quad[3] = { center.x - u.x * half_w + v.x * half_h, center.y - u.y * half_w + v.y * half_h };

        // Scale to original image space
        TextBox tb;
        tb.score = score;
        for (int k = 0; k < 4; ++k) {
            tb.quad.push_back({
                std::clamp(quad[k].x * scale_x, 0.0f, static_cast<float>(orig_w)),
                std::clamp(quad[k].y * scale_y, 0.0f, static_cast<float>(orig_h))
            });
        }
        boxes.push_back(tb);
    }

    // Sort top-to-bottom, left-to-right matching official PaddleOCR sorted_boxes
    std::sort(boxes.begin(), boxes.end(), [](const TextBox& a, const TextBox& b) {
        float y_diff = std::abs(a.quad[0].y - b.quad[0].y);
        if (y_diff > 10.0f) {
            return a.quad[0].y < b.quad[0].y;
        }
        return a.quad[0].x < b.quad[0].x;
    });

    return boxes;
}

// ============================================================================
// CtcDecoder Implementation
// ============================================================================

CtcDecoder::CtcDecoder(const std::string& dict_path) {
    std::ifstream file(dict_path);
    if (!file.is_open()) {
        std::cerr << "[-] Warning: Character dictionary not found at " << dict_path << "\n";
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        dict_.push_back(line);
    }
    std::cout << "[+] Loaded character dictionary: " << dict_.size() << " symbols.\n";
}

std::string CtcDecoder::decode(const uint16_t* data_fp16, int seq_len, int vocab_size, float& out_conf) const {
    std::string text;
    int prev_idx = -1;
    float conf_sum = 0.0f;
    int char_count = 0;

    for (int t = 0; t < seq_len; ++t) {
        const uint16_t* step_slice = data_fp16 + t * vocab_size;

        int best_idx = 0;
        float best_val = -1e9f;

        // FP16 values are ordered by bits after a sign transform. Normalize
        // subnormals because fp16_to_float intentionally maps them to signed zero.
        uint16_t best_key = 0;
        uint16_t best_raw = 0;
        bool have_best = false;
        for (int c = 0; c < vocab_size; ++c) {
            const uint16_t raw = step_slice[c];
            const uint16_t exponent = raw & 0x7c00u;
            const uint16_t mantissa = raw & 0x03ffu;
            if (exponent == 0x7c00u && mantissa != 0) continue; // NaN
            const uint16_t normalized = exponent == 0 ? 0 : raw;
            const uint16_t key = (normalized & 0x8000u)
                ? static_cast<uint16_t>(~normalized)
                : static_cast<uint16_t>(normalized ^ 0x8000u);
            if (!have_best || key > best_key) {
                best_key = key;
                best_raw = normalized;
                best_idx = c;
                have_best = true;
            }
        }
        if (have_best) best_val = fp16_to_float(best_raw);

        // CTC greedy decode: skip blank (0) and consecutive duplicates
        if (best_idx != 0 && best_idx != prev_idx) {
            size_t dict_idx = static_cast<size_t>(best_idx - 1);
            if (dict_idx < dict_.size()) {
                text += dict_[dict_idx];
                conf_sum += best_val;
                char_count++;
            }
        }
        prev_idx = best_idx;
    }

    out_conf = (char_count > 0) ? (conf_sum / char_count) : 0.0f;
    return text;
}

} // namespace ppocr
