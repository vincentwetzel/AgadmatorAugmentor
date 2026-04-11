#include "BoardLocalizer.h"
#include <algorithm>

namespace aa {

// Downscale factor for passes 1-2: 1/4 resolution → 16x faster matchTemplate
static constexpr int DOWNSCALE = 4;

BoardGeometry locate_board(const cv::Mat& img_bgr, const cv::Mat& board_template) {
    BoardGeometry geo;
    double best_scale = 1.0;
    double best_val = -1.0;

    // Downscale image and template for passes 1-2 (coarse + fine)
    cv::Mat img_ds, tpl_ds;
    int ds_w = std::max(img_bgr.cols / DOWNSCALE, 1);
    int ds_h = std::max(img_bgr.rows / DOWNSCALE, 1);
    cv::resize(img_bgr, img_ds, cv::Size(ds_w, ds_h), 0, 0, cv::INTER_AREA);
    int tpl_dw = std::max(board_template.cols / DOWNSCALE, 1);
    int tpl_dh = std::max(board_template.rows / DOWNSCALE, 1);
    cv::resize(board_template, tpl_ds, cv::Size(tpl_dw, tpl_dh), 0, 0, cv::INTER_AREA);

    // Pass 1: Coarse search (0.3x to 1.5x, 25 steps) — 1/4 resolution
    for (int i = 0; i < 25; ++i) {
        double scale = 0.3 + (1.5 - 0.3) * i / 24.0;
        int rw = static_cast<int>(tpl_ds.cols * scale);
        int rh = static_cast<int>(tpl_ds.rows * scale);
        if (rh <= 0 || rw <= 0 || rh > img_ds.rows || rw > img_ds.cols) continue;

        cv::Mat resized;
        cv::resize(tpl_ds, resized, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);

        cv::Mat result;
        cv::matchTemplate(img_ds, resized, result, cv::TM_CCOEFF_NORMED);
        double max_val;
        cv::minMaxLoc(result, nullptr, &max_val);
        if (max_val > best_val) {
            best_val = max_val;
            best_scale = scale;
        }
    }

    // Pass 2: Fine search (best_scale ± 0.05, 21 steps) — 1/4 resolution
    best_val = -1.0;
    for (int i = 0; i < 21; ++i) {
        double scale = best_scale - 0.05 + 0.1 * i / 20.0;
        int rw = static_cast<int>(tpl_ds.cols * scale);
        int rh = static_cast<int>(tpl_ds.rows * scale);
        if (rh <= 0 || rw <= 0 || rh > img_ds.rows || rw > img_ds.cols) continue;

        cv::Mat resized;
        cv::resize(tpl_ds, resized, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);

        cv::Mat result;
        cv::matchTemplate(img_ds, resized, result, cv::TM_CCOEFF_NORMED);
        double max_val;
        cv::minMaxLoc(result, nullptr, &max_val);
        if (max_val > best_val) {
            best_val = max_val;
            best_scale = scale;
        }
    }

    // Scale is invariant to downscaling — pass 3 continues from same scale range

    // Pass 3: Exact search (best_scale ± 0.01, 21 steps) — full resolution
    best_val = -1.0;
    cv::Point best_loc;
    cv::Size best_shape = board_template.size();
    for (int i = 0; i < 21; ++i) {
        double scale = best_scale - 0.01 + 0.02 * i / 20.0;
        int rw = static_cast<int>(board_template.cols * scale);
        int rh = static_cast<int>(board_template.rows * scale);
        if (rh <= 0 || rw <= 0 || rh > img_bgr.rows || rw > img_bgr.cols) continue;

        cv::Mat resized;
        cv::resize(board_template, resized, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);

        cv::Mat result;
        cv::matchTemplate(img_bgr, resized, result, cv::TM_CCOEFF_NORMED);
        double max_val;
        cv::Point max_loc;
        cv::minMaxLoc(result, nullptr, &max_val, nullptr, &max_loc);
        if (max_val > best_val) {
            best_val = max_val;
            best_loc = max_loc;
            best_shape = cv::Size(rw, rh);
        }
    }

    geo.bx = best_loc.x;
    geo.by = best_loc.y;
    geo.bw = best_shape.width;
    geo.bh = best_shape.height;
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

} // namespace aa
