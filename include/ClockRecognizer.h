#pragma once

#include <opencv2/core/mat.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cta {

struct BoardGeometry;

inline constexpr double kClockRoiLeftEdgeRatio = 0.70;
inline constexpr double kClockRoiTopMarginSquares = 0.55;
inline constexpr double kClockRoiTopInsetSquares = 0.08;
inline constexpr double kClockRoiBottomInsetSquares = 0.18;
inline constexpr double kClockRoiBottomMarginSquares = 0.58;

struct ClockRoiBounds {
    int x1 = 0;
    int x2 = 0;
    int top_y1 = 0;
    int top_y2 = 0;
    int bottom_y1 = 0;
    int bottom_y2 = 0;

    bool valid() const {
        return x2 > x1 && top_y2 > top_y1 && bottom_y2 > bottom_y1;
    }
};

/// Computes the shared board-relative clock crop used by the mapper and the
/// full-frame fallback. Keeping this contract in one place makes ROI
/// calibration results comparable to production extraction.
ClockRoiBounds clock_roi_bounds(const BoardGeometry& geo,
                                int frame_width,
                                int frame_height);

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
    std::unordered_map<std::uint64_t, std::string> top_ocr_cache;
    std::unordered_map<std::uint64_t, std::string> bot_ocr_cache;
    bool valid = false;
};

struct ClockOcrDiagnostics {
    std::string preprocessing_variant;
    std::string thresholding_mode;
    std::string selected_reading;
    struct Segment {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        char symbol = '?';
    };
    std::vector<Segment> segments;
    std::vector<std::string> candidates;
};

/// Reconciles repeated OCR observations without converting disagreement into
/// an arbitrary guess. A reading is temporally plausible only when it repeats.
struct ClockTemporalReconciliation {
    std::string selected_reading;
    std::string provenance; // direct, temporally_plausible, missing, inherited, rejected
    std::size_t sample_count = 0;
    std::size_t observed_count = 0;
    std::size_t agreement_count = 0;
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

/// Extracts clocks from already-cropped top and bottom clock pill ROIs.
/// This avoids rebuilding a synthetic full video frame during reducer validation.
ClockState extract_clocks_from_rois(const cv::Mat& top_bgr,
                                    const cv::Mat& bot_bgr,
                                    ClockCache* cache = nullptr);

/// Extracts clocks after an accepted move, OCRing only the player who just moved
/// and reusing the unchanged opponent clock from cache.
ClockState extract_clocks_for_moved_player_from_rois(const cv::Mat& top_bgr,
                                                     const cv::Mat& bot_bgr,
                                                     const std::string& moved_player,
                                                     ClockCache* cache = nullptr,
                                                     const std::string& active_player_hint = "");

/// Returns all unique OCR strings considered for a right-aligned clock ROI.
/// This is used by the reducer when the first OCR choice conflicts with
/// nearby game-state evidence.
std::vector<std::string> recognize_clock_time_candidates_from_roi(const cv::Mat& bgr,
                                                                  bool active_first);

/// Returns calibration provenance for the production candidate search over a
/// single clock ROI. This does not change the selected OCR reading.
ClockOcrDiagnostics diagnose_clock_time_from_roi(const cv::Mat& bgr,
                                                 bool active_first);

ClockTemporalReconciliation reconcile_clock_readings(
    const std::vector<std::string>& readings,
    const std::string& inherited_reading = "");

/// Cheap active-clock detection from cropped clock pill ROIs. This performs no OCR.
std::string detect_active_clock_from_rois(const cv::Mat& top_bgr,
                                          const cv::Mat& bot_bgr);

} // namespace cta
