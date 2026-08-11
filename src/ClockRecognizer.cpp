// Extracted from cpp directory
#include "ClockRecognizer.h"
#include "BoardLocalizer.h"
#include "DigitRecognizer.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace cta {

ClockRoiBounds clock_roi_bounds(const BoardGeometry& geo,
                                int frame_width,
                                int frame_height) {
    ClockRoiBounds bounds;
    bounds.x1 = std::max(0, static_cast<int>(geo.bx + geo.bw * kClockRoiLeftEdgeRatio));
    bounds.x2 = std::min(frame_width, static_cast<int>(geo.bx + geo.bw));
    bounds.top_y1 = std::max(0, static_cast<int>(geo.by - geo.sq_h * kClockRoiTopMarginSquares));
    bounds.top_y2 = std::max(bounds.top_y1 + 1,
                             static_cast<int>(geo.by - geo.sq_h * kClockRoiTopInsetSquares));
    bounds.bottom_y1 = std::min(frame_height - 1,
                                static_cast<int>(geo.by + geo.bh + geo.sq_h * kClockRoiBottomInsetSquares));
    bounds.bottom_y2 = std::min(frame_height,
                                static_cast<int>(geo.by + geo.bh + geo.sq_h * kClockRoiBottomMarginSquares));
    return bounds;
}

ClockTemporalReconciliation reconcile_clock_readings(
    const std::vector<std::string>& readings,
    const std::string& inherited_reading) {
    ClockTemporalReconciliation result;
    result.sample_count = readings.size();
    std::map<std::string, std::size_t> counts;
    for (const auto& reading : readings) {
        if (!reading.empty()) {
            ++result.observed_count;
            ++counts[reading];
        }
    }

    if (counts.empty()) {
        if (!inherited_reading.empty()) {
            result.selected_reading = inherited_reading;
            result.provenance = "inherited";
        } else {
            result.provenance = "missing";
        }
        return result;
    }

    if (counts.size() == 1) {
        const auto& [reading, count] = *counts.begin();
        result.agreement_count = count;
        result.selected_reading = reading;
        result.provenance = result.sample_count == 1
            ? "direct"
            : count >= 2 ? "temporally_plausible" : "rejected";
        if (result.provenance == "rejected") result.selected_reading.clear();
        return result;
    }

    auto best = counts.begin();
    bool tied = false;
    for (auto it = std::next(counts.begin()); it != counts.end(); ++it) {
        if (it->second > best->second) {
            best = it;
            tied = false;
        } else if (it->second == best->second) {
            tied = true;
        }
    }
    if (!tied && best->second >= 2) {
        result.selected_reading = best->first;
        result.agreement_count = best->second;
        result.provenance = "temporally_plausible";
    } else {
        result.provenance = "rejected";
    }
    return result;
}

// ── Clock extraction ─────────────────────────────────────────────────────────

std::string detect_active_clock_from_rois(const cv::Mat& top_bgr,
                                          const cv::Mat& bot_bgr) {
    if (top_bgr.empty() || bot_bgr.empty()) {
        return "";
    }

    auto count_white = [](const cv::Mat& roi) {
        int white = 0;
        for (int y = 0; y < roi.rows; ++y) {
            const cv::Vec3b* row = roi.ptr<cv::Vec3b>(y);
            for (int x = 0; x < roi.cols; ++x) {
                const cv::Vec3b& p = row[x];
                int lum = (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) / 3;
                if (lum > 200) {
                    ++white;
                }
            }
        }
        return white;
    };

    int top_white = count_white(top_bgr);
    int bot_white = count_white(bot_bgr);
    if (top_white < 50 && bot_white < 50) {
        return "";
    }
    return (bot_white > top_white) ? "white" : "black";
}

static cv::Mat crop_right_aligned_clock_text(const cv::Mat& bgr, double left_ratio = 0.12) {
    int x1 = std::clamp(static_cast<int>(bgr.cols * left_ratio), 0, std::max(0, bgr.cols - 1));
    int width = bgr.cols - x1;
    if (width <= 0) {
        return bgr;
    }
    return bgr(cv::Rect(x1, 0, width, bgr.rows));
}

static bool looks_like_clock_string(const std::string& s) {
    return s.length() >= 3 &&
           (s.find(':') != std::string::npos || s.find('.') != std::string::npos);
}

static bool looks_like_full_clock_string(const std::string& s) {
    return s.length() >= 5 &&
           std::count(s.begin(), s.end(), ':') >= 1;
}

static std::uint64_t clock_roi_fingerprint(const cv::Mat& bgr) {
    if (bgr.empty()) {
        return 0;
    }

    cv::Mat gray;
    cv::cvtColor(crop_right_aligned_clock_text(bgr), gray, cv::COLOR_BGR2GRAY);

    cv::Mat small;
    cv::resize(gray, small, cv::Size(32, 12), 0, 0, cv::INTER_AREA);

    std::uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < small.rows; ++y) {
        const uchar* row = small.ptr<uchar>(y);
        for (int x = 0; x < small.cols; ++x) {
            h ^= static_cast<std::uint64_t>(row[x] >> 3);
            h *= 1099511628211ull;
        }
    }
    return h;
}

static std::string recognize_clock_time_with_hint(const cv::Mat& bgr, bool active_first) {
    static constexpr std::array<double, 6> active_left_ratios{0.30, 0.40, 0.50, 0.55, 0.60, 0.12};
    static constexpr std::array<double, 6> inactive_left_ratios{0.12, 0.30, 0.40, 0.50, 0.55, 0.60};
    const auto& left_ratios = active_first ? active_left_ratios : inactive_left_ratios;
    std::string fallback;
    for (double left_ratio : left_ratios) {
        cv::Mat time_text = crop_right_aligned_clock_text(bgr, left_ratio);
        std::string res = DigitRecognizer::recognize_time(time_text, active_first);
        if (looks_like_full_clock_string(res)) {
            return res;
        }
        if (fallback.empty() && looks_like_clock_string(res)) fallback = res;
        res = DigitRecognizer::recognize_time(time_text, !active_first);
        if (looks_like_full_clock_string(res)) {
            return res;
        }
        if (fallback.empty() && looks_like_clock_string(res)) fallback = res;
    }

    std::string res = DigitRecognizer::recognize_time(bgr, active_first);
    if (looks_like_full_clock_string(res)) {
        return res;
    }
    if (fallback.empty() && looks_like_clock_string(res)) fallback = res;
    res = DigitRecognizer::recognize_time(bgr, !active_first);
    if (looks_like_full_clock_string(res)) {
        return res;
    }
    if (fallback.empty() && looks_like_clock_string(res)) fallback = res;
    return fallback;
}

std::vector<std::string> recognize_clock_time_candidates_from_roi(const cv::Mat& bgr,
                                                                  bool active_first) {
    static constexpr std::array<double, 6> active_left_ratios{0.30, 0.40, 0.50, 0.55, 0.60, 0.12};
    static constexpr std::array<double, 6> inactive_left_ratios{0.12, 0.30, 0.40, 0.50, 0.55, 0.60};
    const auto& left_ratios = active_first ? active_left_ratios : inactive_left_ratios;

    std::vector<std::string> candidates;
    auto add_candidate = [&](const std::string& value) {
        if (!looks_like_clock_string(value)) {
            return;
        }
        if (std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
            candidates.push_back(value);
        }
    };

    for (double left_ratio : left_ratios) {
        cv::Mat time_text = crop_right_aligned_clock_text(bgr, left_ratio);
        add_candidate(DigitRecognizer::recognize_time(time_text, active_first));
        add_candidate(DigitRecognizer::recognize_time(time_text, !active_first));
    }

    add_candidate(DigitRecognizer::recognize_time(bgr, active_first));
    add_candidate(DigitRecognizer::recognize_time(bgr, !active_first));
    return candidates;
}

ClockOcrDiagnostics diagnose_clock_time_from_roi(const cv::Mat& bgr,
                                                 bool active_first) {
    static constexpr std::array<double, 6> active_left_ratios{0.30, 0.40, 0.50, 0.55, 0.60, 0.12};
    static constexpr std::array<double, 6> inactive_left_ratios{0.12, 0.30, 0.40, 0.50, 0.55, 0.60};
    const auto& left_ratios = active_first ? active_left_ratios : inactive_left_ratios;

    ClockOcrDiagnostics result;
    auto copy_diagnostics = [&](const DigitRecognizer::RecognitionDiagnostics& source,
                                const std::string& reading,
                                const std::string& roi_variant) {
        result.preprocessing_variant = source.preprocessing_variant + ":" + roi_variant;
        result.thresholding_mode = source.thresholding_mode;
        result.selected_reading = reading;
        result.segments.clear();
        result.segments.reserve(source.segments.size());
        for (const auto& segment : source.segments) {
            result.segments.push_back({segment.x, segment.y, segment.width,
                                       segment.height, segment.symbol});
        }
    };

    std::string fallback;
    DigitRecognizer::RecognitionDiagnostics fallback_diagnostics;
    double fallback_ratio = left_ratios.front();
    for (double left_ratio : left_ratios) {
        const cv::Mat time_text = crop_right_aligned_clock_text(bgr, left_ratio);
        DigitRecognizer::RecognitionDiagnostics diagnostics;
        std::string reading = DigitRecognizer::recognize_time_with_diagnostics(
            time_text, active_first, &diagnostics);
        if (std::find(result.candidates.begin(), result.candidates.end(), reading) ==
            result.candidates.end() && looks_like_clock_string(reading)) {
            result.candidates.push_back(reading);
        }
        if (looks_like_full_clock_string(reading)) {
            copy_diagnostics(diagnostics, reading,
                             "right_aligned_" +
                                 std::to_string(static_cast<int>(left_ratio * 100.0)) + "pct");
            return result;
        }
        if (fallback.empty() && looks_like_clock_string(reading)) {
            fallback = reading;
            fallback_diagnostics = diagnostics;
            fallback_ratio = left_ratio;
        }

        diagnostics = {};
        reading = DigitRecognizer::recognize_time_with_diagnostics(
            time_text, !active_first, &diagnostics);
        if (std::find(result.candidates.begin(), result.candidates.end(), reading) ==
            result.candidates.end() && looks_like_clock_string(reading)) {
            result.candidates.push_back(reading);
        }
        if (looks_like_full_clock_string(reading)) {
            copy_diagnostics(diagnostics, reading,
                             "right_aligned_" +
                                 std::to_string(static_cast<int>(left_ratio * 100.0)) + "pct");
            return result;
        }
        if (fallback.empty() && looks_like_clock_string(reading)) {
            fallback = reading;
            fallback_diagnostics = diagnostics;
            fallback_ratio = left_ratio;
        }
    }

    for (bool hint : {active_first, !active_first}) {
        DigitRecognizer::RecognitionDiagnostics diagnostics;
        const std::string reading = DigitRecognizer::recognize_time_with_diagnostics(
            bgr, hint, &diagnostics);
        if (std::find(result.candidates.begin(), result.candidates.end(), reading) ==
            result.candidates.end() && looks_like_clock_string(reading)) {
            result.candidates.push_back(reading);
        }
        if (looks_like_full_clock_string(reading)) {
            copy_diagnostics(diagnostics, reading, "full_roi");
            return result;
        }
        if (fallback.empty() && looks_like_clock_string(reading)) {
            fallback = reading;
            fallback_diagnostics = diagnostics;
            fallback_ratio = 1.0;
        }
    }

    if (!fallback.empty()) {
        const std::string roi_variant = fallback_ratio == 1.0
            ? "full_roi"
            : "right_aligned_" +
                std::to_string(static_cast<int>(fallback_ratio * 100.0)) + "pct";
        copy_diagnostics(fallback_diagnostics, fallback, roi_variant);
    }
    return result;
}

static std::string cached_recognize_clock_time(const cv::Mat& bgr,
                                               bool active_first,
                                               std::unordered_map<std::uint64_t, std::string>* cache) {
    if (!cache) {
        return recognize_clock_time_with_hint(bgr, active_first);
    }

    std::uint64_t key = clock_roi_fingerprint(bgr);
    auto it = cache->find(key);
    if (it != cache->end()) {
        return it->second;
    }

    std::string res = recognize_clock_time_with_hint(bgr, active_first);
    if (cache->size() > 512) {
        cache->clear();
    }
    cache->emplace(key, res);
    return res;
}

ClockState extract_clocks_for_moved_player_from_rois(const cv::Mat& top_bgr,
                                                     const cv::Mat& bot_bgr,
                                                     const std::string& moved_player,
                                                     ClockCache* cache,
                                                     const std::string& active_player_hint) {
    if (top_bgr.empty() || bot_bgr.empty()) {
        return {};
    }

    ClockState state;
    state.active_player = active_player_hint.empty()
        ? detect_active_clock_from_rois(top_bgr, bot_bgr)
        : active_player_hint;
    if (cache && cache->valid) {
        state.white_time = cache->white_time;
        state.black_time = cache->black_time;
    }

    if (moved_player == "white") {
        std::string recognized = cached_recognize_clock_time(
            bot_bgr, false, cache ? &cache->bot_ocr_cache : nullptr);
        if (looks_like_clock_string(recognized)) {
            state.white_time = recognized;
        }
    } else if (moved_player == "black") {
        std::string recognized = cached_recognize_clock_time(
            top_bgr, false, cache ? &cache->top_ocr_cache : nullptr);
        if (looks_like_clock_string(recognized)) {
            state.black_time = recognized;
        }
        if (state.black_time.empty() && state.active_player == "black") {
            cv::Mat tight_top_text = crop_right_aligned_clock_text(top_bgr, 0.40);
            state.black_time = DigitRecognizer::recognize_time(tight_top_text, true);
            if (state.black_time.empty()) {
                state.black_time = DigitRecognizer::recognize_time(tight_top_text, false);
            }
        }
    }

    state.ocr_skipped = false;
    if (cache) {
        if (moved_player == "white") {
            cv::cvtColor(bot_bgr, cache->bot_gray, cv::COLOR_BGR2GRAY);
        } else if (moved_player == "black") {
            cv::cvtColor(top_bgr, cache->top_gray, cv::COLOR_BGR2GRAY);
        }
        cache->white_time = state.white_time;
        cache->black_time = state.black_time;
        cache->valid = true;
    }
    return state;
}

ClockState extract_clocks_from_rois(const cv::Mat& top_bgr,
                                    const cv::Mat& bot_bgr,
                                    ClockCache* cache) {
    if (top_bgr.empty() || bot_bgr.empty()) {
        return {};
    }

    ClockState state;
    state.active_player = detect_active_clock_from_rois(top_bgr, bot_bgr);

    // Conditional OCR cache
    cv::Mat top_gray, bot_gray;
    cv::cvtColor(top_bgr, top_gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(bot_bgr, bot_gray, cv::COLOR_BGR2GRAY);

    bool need_ocr = true;
    if (cache && cache->valid) {
        double top_diff = 0, bot_diff = 0;
        if (top_gray.size() == cache->top_gray.size()) {
            cv::Mat d;
            cv::absdiff(top_gray, cache->top_gray, d);
            top_diff = cv::mean(d)[0];
        }
        if (bot_gray.size() == cache->bot_gray.size()) {
            cv::Mat d;
            cv::absdiff(bot_gray, cache->bot_gray, d);
            bot_diff = cv::mean(d)[0];
        }

        if (top_diff < 5.0 && bot_diff < 5.0) {
            state.white_time = cache->white_time;
            state.black_time = cache->black_time;
            state.ocr_skipped = true;
            need_ocr = false;
        }
    }

    if (need_ocr) {
        bool top_active_first = state.active_player == "black";
        bool bot_active_first = state.active_player == "white";
        std::string recognized_white = cached_recognize_clock_time(
            bot_bgr, bot_active_first, cache ? &cache->bot_ocr_cache : nullptr);
        if (looks_like_clock_string(recognized_white)) {
            state.white_time = recognized_white;
        }
        std::string recognized_black = cached_recognize_clock_time(
            top_bgr, top_active_first, cache ? &cache->top_ocr_cache : nullptr);
        if (looks_like_clock_string(recognized_black)) {
            state.black_time = recognized_black;
        }

        if (state.black_time.empty() && state.active_player == "black") {
            auto tight_top_text = top_bgr(cv::Rect(
                std::clamp(static_cast<int>(top_bgr.cols * 0.40), 0, std::max(0, top_bgr.cols - 1)),
                0,
                top_bgr.cols - std::clamp(static_cast<int>(top_bgr.cols * 0.40), 0, std::max(0, top_bgr.cols - 1)),
                top_bgr.rows));
        state.black_time = DigitRecognizer::recognize_time(tight_top_text, true);
            if (state.black_time.empty()) {
            state.black_time = DigitRecognizer::recognize_time(tight_top_text, false);
            }
        }

        state.ocr_skipped = false;

        if (cache) {
            cache->top_gray = top_gray;
            cache->bot_gray = bot_gray;
            cache->white_time = state.white_time;
            cache->black_time = state.black_time;
            cache->valid = true;
        }
    }

    return state;
}

ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo,
                          ClockCache* cache) {
    (void)board_template;

    // The chess.com clocks are right-aligned to the board, so keep the ROI
    // tight around the pill instead of sweeping a broad strip that includes UI text.
    const ClockRoiBounds bounds = clock_roi_bounds(geo, img_bgr.cols, img_bgr.rows);
    if (!bounds.valid()) {
        return {};
    }

    cv::Mat top_roi = img_bgr(cv::Rect(
        bounds.x1, bounds.top_y1, bounds.x2 - bounds.x1,
        bounds.top_y2 - bounds.top_y1));
    cv::Mat bot_roi = img_bgr(cv::Rect(
        bounds.x1, bounds.bottom_y1, bounds.x2 - bounds.x1,
        bounds.bottom_y2 - bounds.bottom_y1));
    return extract_clocks_from_rois(top_roi, bot_roi, cache);
}

} // namespace cta
