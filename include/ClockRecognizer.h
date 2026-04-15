#pragma once

#include <opencv2/opencv.hpp>
#include <string>

namespace aa {

struct BoardGeometry;

/// Result of clock extraction.
struct ClockState {
    std::string active_player; // "white", "black", or empty string if neither
    std::string white_time;    // e.g. "10:00" or "1:31:28"
    std::string black_time;
    bool ocr_skipped = false;  // true if times were reused from cache (no OCR ran)
};

/// Cache for conditional clock OCR — holds previous clock ROI grayscale images.
/// When clock pixels haven't meaningfully changed, OCR is skipped and cached
/// times are reused.
struct ClockCache {
    cv::Mat top_gray;    // Previous top clock ROI (grayscale)
    cv::Mat bot_gray;    // Previous bottom clock ROI (grayscale)
    std::string white_time;
    std::string black_time;
    bool valid = false;
};

// ── Clock extraction ─────────────────────────────────────────────────────────

/// Extracts the active player and remaining time from both clock pills.
/// Uses a Hu Moments-based digit recognizer with pre-computed 7-segment
/// display templates — zero external dependencies, runs in microseconds.
/// @param cache  Optional cache for conditional OCR. If provided and clock
///               pixels haven't meaningfully changed, OCR is skipped and
///               cached times are reused. Cache is updated on OCR runs.
ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo,
                          ClockCache* cache = nullptr);

} // namespace aa
