#pragma once

#include <opencv2/core.hpp>

namespace cta {

/// Result of board localization.
struct BoardGeometry {
    int bx = 0, by = 0;        // Top-left corner (x, y)
    int bw = 0, bh = 0;        // Board width, height in pixels
    double sq_w = 0.0, sq_h = 0.0; // Square width, height in pixels
    double localization_score = 0.0; // Final template correlation, [-1, 1]
    double localization_scale = 0.0; // Template scale selected by the final pass
    // Normalized confidence derived only from the final template correlation.
    // It is evidence for downstream diagnostics, not a move-selection veto.
    double geometry_confidence = 0.0;
};

/// Returns normalized geometry uncertainty in [0, 1]. Zero means the
/// localized geometry has full template confidence; one means unavailable or
/// completely untrusted geometry. This is advisory metadata for detectors.
double geometry_uncertainty(const BoardGeometry& geo);

struct GeometryStability {
    bool stable = true;
    bool anchor_drift_exceeded = false;
    bool step_drift_exceeded = false;
    double anchor_position_drift_pixels = 0.0;
    double anchor_size_drift_pixels = 0.0;
    double step_position_drift_pixels = 0.0;
    double step_size_drift_pixels = 0.0;
};

/// Compares a fresh localization with the anchored geometry and, when
/// available, the preceding probe. The result is an evidence guard rather
/// than a localization replacement because mapper frames use the anchor.
GeometryStability assess_geometry_stability(
    const BoardGeometry& anchor,
    const BoardGeometry& observed,
    const BoardGeometry* previous = nullptr);

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

} // namespace cta
