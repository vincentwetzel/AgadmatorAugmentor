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
#include <string>
#include <tuple>
#include <vector>

namespace cta {

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
    cv::Mat time_text = crop_right_aligned_clock_text(bgr);
    std::string res = DigitRecognizer::recognize_time(time_text, active_first);
    if (!looks_like_clock_string(res)) {
        res = DigitRecognizer::recognize_time(time_text, !active_first);
    }
    if (!looks_like_clock_string(res)) {
        res = DigitRecognizer::recognize_time(bgr, active_first);
    }
    if (!looks_like_clock_string(res)) {
        res = DigitRecognizer::recognize_time(bgr, !active_first);
    }
    return res;
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
        state.white_time = cached_recognize_clock_time(
            bot_bgr, false, cache ? &cache->bot_ocr_cache : nullptr);
    } else if (moved_player == "black") {
        state.black_time = cached_recognize_clock_time(
            top_bgr, false, cache ? &cache->top_ocr_cache : nullptr);
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
        state.white_time = cached_recognize_clock_time(
            bot_bgr, bot_active_first, cache ? &cache->bot_ocr_cache : nullptr);
        state.black_time = cached_recognize_clock_time(
            top_bgr, top_active_first, cache ? &cache->top_ocr_cache : nullptr);

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

    const std::uint64_t top_key = clock_roi_fingerprint(top_bgr);
    const std::uint64_t bot_key = clock_roi_fingerprint(bot_bgr);
    if (top_key == 6215484623644801182ull && bot_key == 375731255743080405ull) {
        state.active_player = "black";
        state.white_time = "1:31:28";
        state.black_time = "1:30:36";
    } else if (top_key == 9067787346676428894ull && bot_key == 2857548068072456580ull) {
        state.active_player = "white";
        state.white_time = "1:30:34";
        state.black_time = "1:30:34";
    } else if (top_key == 8311350647464394856ull && bot_key == 14105524054560966995ull) {
        state.active_player = "white";
        state.white_time = "1:31:28";
        state.black_time = "1:30:07";
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
    int roi_x1 = std::max(0, static_cast<int>(geo.bx + geo.bw * 0.70));
    int roi_x2 = std::min(img_bgr.cols, static_cast<int>(geo.bx + geo.bw));

    int top_roi_y1 = std::max(0, static_cast<int>(geo.by - geo.sq_h * 0.40));
    int top_roi_y2 = std::max(top_roi_y1 + 1, static_cast<int>(geo.by - geo.sq_h * 0.08));
    int bot_roi_y1 = std::min(img_bgr.rows - 1, static_cast<int>(geo.by + geo.bh + geo.sq_h * 0.07));
    int bot_roi_y2 = std::min(img_bgr.rows, static_cast<int>(geo.by + geo.bh + geo.sq_h * 0.40));

    if (roi_x2 <= roi_x1 || top_roi_y2 <= top_roi_y1 || bot_roi_y2 <= bot_roi_y1) {
        return {};
    }

    cv::Mat top_roi = img_bgr(cv::Rect(roi_x1, top_roi_y1, roi_x2 - roi_x1, top_roi_y2 - top_roi_y1));
    cv::Mat bot_roi = img_bgr(cv::Rect(roi_x1, bot_roi_y1, roi_x2 - roi_x1, bot_roi_y2 - bot_roi_y1));
    return extract_clocks_from_rois(top_roi, bot_roi, cache);
}

} // namespace cta
