#pragma once

#include <opencv2/opencv.hpp>

namespace aa {

/// Result of board localization.
struct BoardGeometry {
    int bx = 0, by = 0;        // Top-left corner (x, y)
    int bw = 0, bh = 0;        // Board width, height in pixels
    double sq_w = 0.0, sq_h = 0.0; // Square width, height in pixels
};

/// Performs multi-pass template matching to find the exact board coordinates and scale.
///
/// Three sequential passes: coarse (0.3x–1.5x, 25 steps) → fine (±0.05, 21 steps) → exact (±0.01, 21 steps).
/// Returns a BoardGeometry with the top-left corner, board dimensions, and per-square size.
BoardGeometry locate_board(const cv::Mat& img_bgr, const cv::Mat& board_template);

/// Draws an 8x8 grid on the image, with optional highlighting and labels.
void draw_board_grid(cv::Mat& image, const BoardGeometry& geo,
                     const cv::Scalar& default_color = cv::Scalar(0, 255, 0),
                     int thickness = 2,
                     bool draw_labels = false);

} // namespace aa
