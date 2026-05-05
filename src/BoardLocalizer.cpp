// Extracted from cpp directory
#include "BoardLocalizer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

namespace cta {

// Downscale factor for passes 1-2: 1/4 resolution → 16x faster matchTemplate
static constexpr int DOWNSCALE = 4;

// Golden Section Search constants
static constexpr double GOLDEN_RATIO = 1.618033988749895;
static constexpr double INV_PHI = 0.618033988749895; // 1/φ = φ - 1

static cv::Mat make_external_mat(int rows, int cols, int type, std::vector<unsigned char>& storage) {
    storage.resize(static_cast<size_t>(rows) * static_cast<size_t>(cols) * CV_ELEM_SIZE(type));
    return cv::Mat(rows, cols, type, storage.data());
}

static cv::Mat to_gray_external(const cv::Mat& src, std::vector<unsigned char>& storage) {
    cv::Mat gray = make_external_mat(src.rows, src.cols, CV_8UC1, storage);
    if (src.channels() == 1) {
        src.copyTo(gray);
        return gray;
    }

    for (int y = 0; y < src.rows; ++y) {
        const uchar* s = src.ptr<uchar>(y);
        uchar* d = gray.ptr<uchar>(y);
        for (int x = 0; x < src.cols; ++x) {
            const int b = s[x * src.channels() + 0];
            const int g = s[x * src.channels() + 1];
            const int r = s[x * src.channels() + 2];
            d[x] = static_cast<uchar>((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
    return gray;
}

struct SearchWindow {
    int x0 = 0;
    int y0 = 0;
    int x1 = std::numeric_limits<int>::max();
    int y1 = std::numeric_limits<int>::max();
    int stride = 1;
};

// Evaluates TM_CCOEFF_NORMED peak correlation for a given scale.
// Returns {max_correlation, best_x, best_y, resized_width, resized_height}.
// Returns correlation of -2.0 if the scale is invalid for these dimensions.
static std::tuple<double, int, int, int, int>
eval_scale(const cv::Mat& img, const cv::Mat& tpl, double scale, SearchWindow window = {}) {
    int rw = static_cast<int>(std::round(tpl.cols * scale));
    int rh = static_cast<int>(std::round(tpl.rows * scale));
    if (rh <= 2 || rw <= 2 || rh > img.rows || rw > img.cols) {
        return {-2.0, 0, 0, 0, 0}; // Invalid — guaranteed to lose
    }

    std::vector<unsigned char> resized_storage;
    cv::Mat resized = make_external_mat(rh, rw, tpl.type(), resized_storage);
    cv::resize(tpl, resized, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);

    const int max_x = img.cols - rw;
    const int max_y = img.rows - rh;
    const int x0 = std::clamp(window.x0, 0, max_x);
    const int y0 = std::clamp(window.y0, 0, max_y);
    const int x1 = std::clamp(window.x1, x0, max_x);
    const int y1 = std::clamp(window.y1, y0, max_y);
    const int stride = std::max(1, window.stride);

    const int grid = std::clamp(std::min(rw, rh) / 8, 12, 28);
    std::vector<cv::Point> samples;
    samples.reserve(static_cast<size_t>(grid) * grid);
    for (int gy = 0; gy < grid; ++gy) {
        int sy = std::clamp((gy * rh + rh / 2) / grid, 0, rh - 1);
        for (int gx = 0; gx < grid; ++gx) {
            int sx = std::clamp((gx * rw + rw / 2) / grid, 0, rw - 1);
            samples.emplace_back(sx, sy);
        }
    }

    const double n = static_cast<double>(samples.size());
    double tpl_sum = 0.0;
    double tpl_sum2 = 0.0;
    std::vector<double> tpl_values;
    tpl_values.reserve(samples.size());
    for (const auto& p : samples) {
        double v = resized.ptr<uchar>(p.y)[p.x];
        tpl_values.push_back(v);
        tpl_sum += v;
        tpl_sum2 += v * v;
    }
    const double tpl_var = std::max(1e-9, tpl_sum2 - (tpl_sum * tpl_sum) / n);

    double best_corr = -2.0;
    int best_x = x0;
    int best_y = y0;
    for (int y = y0; y <= y1; y += stride) {
        for (int x = x0; x <= x1; x += stride) {
            double img_sum = 0.0;
            double img_sum2 = 0.0;
            double cross = 0.0;
            for (size_t i = 0; i < samples.size(); ++i) {
                const auto& p = samples[i];
                const double iv = img.ptr<uchar>(y + p.y)[x + p.x];
                img_sum += iv;
                img_sum2 += iv * iv;
                cross += iv * tpl_values[i];
            }
            const double img_var = img_sum2 - (img_sum * img_sum) / n;
            if (img_var <= 1e-9) continue;
            const double centered_cross = cross - (img_sum * tpl_sum) / n;
            const double corr = centered_cross / std::sqrt(img_var * tpl_var);
            if (corr > best_corr) {
                best_corr = corr;
                best_x = x;
                best_y = y;
            }
        }
    }
    return {best_corr, best_x, best_y, rw, rh};
}

// Golden Section Search result — defined before both search functions
struct GSSResult {
    double best_scale;
    double best_corr;
    int best_x, best_y;
    int best_w, best_h;
};

// Linear fallback: brute-force sweep for a given scale range.
// Used when GSS fails to find any valid correlation.
static GSSResult linear_scale_search(
    const cv::Mat& img, const cv::Mat& tpl,
    double lo, double hi, int steps, SearchWindow window = {})
{
    GSSResult best{};
    best.best_corr = -1.0;
    for (int i = 0; i < steps; ++i) {
        double scale = lo + (hi - lo) * i / std::max(1, steps - 1);
        auto [corr, x, y, w, h] = eval_scale(img, tpl, scale, window);
        if (corr > best.best_corr) {
            best.best_scale = scale;
            best.best_corr = corr;
            best.best_x = x; best.best_y = y;
            best.best_w = w; best.best_h = h;
        }
    }
    return best;
}

// Golden Section Search for the scale that maximizes TM_CCOEFF_NORMED correlation.
// Converges on the unimodal peak in O(log N) iterations instead of linear sweep.
// Falls back to linear search if both initial bracket points are invalid.
static GSSResult golden_section_scale_search(
    const cv::Mat& img, const cv::Mat& tpl,
    double lo, double hi, int iterations, SearchWindow window = {})
{
    double inv_phi = INV_PHI;
    double inv_phi2 = inv_phi * inv_phi;

    // Initialize interior points
    double a = lo, b = hi;
    double h = b - a;

    // Initial bracketing width
    double c = a + inv_phi2 * h;
    double d = a + inv_phi * h;

    auto [fc, xc, yc, wc, hc] = eval_scale(img, tpl, c, window);
    auto [fd, xd, yd, wd, hd] = eval_scale(img, tpl, d, window);

    // If both initial points are invalid, fall back to linear search
    if (fc < -1.0 && fd < -1.0) {
        return linear_scale_search(img, tpl, lo, hi, iterations, window);
    }

    int n = iterations;
    double best_scale = (fc > fd) ? c : d;
    double best_corr = std::max(fc, fd);
    int best_x = (fc > fd) ? xc : xd;
    int best_y = (fc > fd) ? yc : yd;
    int best_w = (fc > fd) ? wc : wd;
    int best_h = (fc > fd) ? hc : hd;

    for (int i = 0; i < n - 2; ++i) {
        if (fc > fd) {
            // Peak is in [a, d], shrink to [a, d]
            b = d;
            d = c;
            fd = fc;
            xd = xc; yd = yc; wd = wc; hd = hc;
            h = inv_phi * h;
            c = a + inv_phi2 * h;
            auto [new_fc, new_xc, new_yc, new_wc, new_hc] = eval_scale(img, tpl, c, window);
            fc = new_fc; xc = new_xc; yc = new_yc; wc = new_wc; hc = new_hc;
        } else {
            // Peak is in [c, b], shrink to [c, b]
            a = c;
            c = d;
            fc = fd;
            xc = xd; yc = yd; wc = wd; hc = hd;
            h = inv_phi * h;
            d = a + inv_phi * h;
            auto [new_fd, new_xd, new_yd, new_wd, new_hd] = eval_scale(img, tpl, d, window);
            fd = new_fd; xd = new_xd; yd = new_yd; wd = new_wd; hd = new_hd;
        }

        // Track best seen across all iterations
        if (fc > best_corr) {
            best_scale = c; best_corr = fc;
            best_x = xc; best_y = yc; best_w = wc; best_h = hc;
        }
        if (fd > best_corr) {
            best_scale = d; best_corr = fd;
            best_x = xd; best_y = yd; best_w = wd; best_h = hd;
        }
    }

    return {best_scale, best_corr, best_x, best_y, best_w, best_h};
}

BoardGeometry locate_board(const cv::Mat& img_bgr, const cv::Mat& board_template) {
    BoardGeometry geo;

    std::vector<unsigned char> img_gray_storage, tpl_gray_storage;
    cv::Mat img_gray = to_gray_external(img_bgr, img_gray_storage);
    cv::Mat tpl_gray = to_gray_external(board_template, tpl_gray_storage);

    // Downscale image and template for passes 1-2 (coarse + fine)
    cv::Mat img_ds, tpl_ds;
    std::vector<unsigned char> img_ds_storage, tpl_ds_storage;
    int ds_w = std::max(img_gray.cols / DOWNSCALE, 1);
    int ds_h = std::max(img_gray.rows / DOWNSCALE, 1);
    img_ds = make_external_mat(ds_h, ds_w, img_gray.type(), img_ds_storage);
    cv::resize(img_gray, img_ds, cv::Size(ds_w, ds_h), 0, 0, cv::INTER_AREA);
    int tpl_dw = std::max(tpl_gray.cols / DOWNSCALE, 1);
    int tpl_dh = std::max(tpl_gray.rows / DOWNSCALE, 1);
    tpl_ds = make_external_mat(tpl_dh, tpl_dw, tpl_gray.type(), tpl_ds_storage);
    cv::resize(tpl_gray, tpl_ds, cv::Size(tpl_dw, tpl_dh), 0, 0, cv::INTER_AREA);

    // Pass 1: Coarse Golden Section Search (0.3x to 1.5x) — 1/4 resolution
    // 15 GSS iterations reduce interval by 0.618^15 ≈ 0.0007 → sub-pixel precision
    SearchWindow coarse_window{0, 0, img_ds.cols - 1, img_ds.rows - 1, 2};
    auto r1 = golden_section_scale_search(img_ds, tpl_ds, 0.3, 1.5, 15, coarse_window);
    if (r1.best_w <= 0 || r1.best_h <= 0) {
        // GSS failed entirely — fall back to wider linear sweep
        r1 = linear_scale_search(img_ds, tpl_ds, 0.1, 3.0, 40, coarse_window);
    }
    double best_scale = r1.best_scale;

    // Pass 2: Fine Golden Section Search (best_scale ± 0.05) — 1/4 resolution
    double lo2 = std::max(0.1, best_scale - 0.05);
    double hi2 = best_scale + 0.05;
    SearchWindow fine_window{r1.best_x - 24, r1.best_y - 24, r1.best_x + 24, r1.best_y + 24, 1};
    auto r2 = golden_section_scale_search(img_ds, tpl_ds, lo2, hi2, 12, fine_window);
    if (r2.best_w <= 0 || r2.best_h <= 0) {
        r2 = linear_scale_search(img_ds, tpl_ds, std::max(0.1, best_scale - 0.2), best_scale + 0.2, 30, fine_window);
    }
    best_scale = r2.best_scale;

    // Pass 3: Exact Golden Section Search (best_scale ± 0.01) — full resolution
    double lo3 = std::max(0.1, best_scale - 0.01);
    double hi3 = best_scale + 0.01;
    SearchWindow exact_window{r2.best_x * DOWNSCALE - 64,
                              r2.best_y * DOWNSCALE - 64,
                              r2.best_x * DOWNSCALE + 64,
                              r2.best_y * DOWNSCALE + 64,
                              1};
    auto r3 = golden_section_scale_search(img_gray, tpl_gray, lo3, hi3, 12, exact_window);
    if (r3.best_w <= 0 || r3.best_h <= 0) {
        r3 = linear_scale_search(img_gray, tpl_gray, std::max(0.1, best_scale - 0.05), best_scale + 0.05, 30, exact_window);
    }

    geo.bx = r3.best_x;
    geo.by = r3.best_y;
    geo.bw = r3.best_w;
    geo.bh = r3.best_h;
    geo.sq_w = static_cast<double>(geo.bw) / 8.0;
    geo.sq_h = static_cast<double>(geo.bh) / 8.0;

    return geo;
}

void draw_board_grid(cv::Mat& image, const BoardGeometry& geo,
                     const cv::Scalar& default_color,
                     int thickness, bool draw_labels) {
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = static_cast<int>(geo.by + row * geo.sq_h);
            int y2 = static_cast<int>(geo.by + (row + 1) * geo.sq_h);
            int x1 = static_cast<int>(geo.bx + col * geo.sq_w);
            int x2 = static_cast<int>(geo.bx + (col + 1) * geo.sq_w);

            cv::rectangle(image, cv::Point(x1, y1), cv::Point(x2, y2), default_color, thickness);

            if (draw_labels) {
                char sq_name[3];
                sq_name[0] = 'a' + col;
                sq_name[1] = '0' + (8 - row);
                sq_name[2] = '\0';
                cv::putText(image, sq_name, cv::Point(x1 + 5, y1 + static_cast<int>(geo.sq_h / 2)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
            }
        }
    }
}

} // namespace cta
