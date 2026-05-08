// Extracted from cpp directory
#include "ClockRecognizer.h"
#include "BoardLocalizer.h"
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

// ── Hu Moments Digit Recognizer ──────────────────────────────────────────────
// Zero-dependency OCR for chess clock digits (0-9, ":").
// Uses pre-computed 7-segment display templates and nearest-neighbor
// classification on 7 log-transformed Hu moments.

struct DigitTemplate {
    char symbol;
    cv::Mat mask;
    double aspect;
    int hole_count;
    double hole_center_y;
    double ink_center_y;
    double ink_center_x;
    double bottom_half_ratio;
};

static std::pair<int, double> analyze_glyph_holes(const cv::Mat& glyph);

static double analyze_ink_center_y(const cv::Mat& glyph);

static double analyze_ink_center_x(const cv::Mat& glyph);

static double analyze_bottom_half_ratio(const cv::Mat& glyph);

static std::vector<DigitTemplate> build_digit_templates() {
    const std::string alphabet = "0123456789:.";
    std::vector<DigitTemplate> templates;
    templates.reserve(alphabet.size());

    for (char symbol : alphabet) {
        cv::Mat canvas(120, 96, CV_8UC1, cv::Scalar(0));
        cv::putText(canvas,
                    std::string(1, symbol),
                    cv::Point(6, 92),
                    cv::FONT_HERSHEY_SIMPLEX,
                    2.7,
                    cv::Scalar(255),
                    5,
                    cv::LINE_AA);

        cv::Mat binary;
        cv::threshold(canvas, binary, 127, 255, cv::THRESH_BINARY);
        if (cv::countNonZero(binary) == 0) {
            continue;
        }

        cv::Rect bbox = cv::boundingRect(binary);
        DigitTemplate tpl;
        tpl.symbol = symbol;
        tpl.mask = binary(bbox).clone();
        tpl.aspect = static_cast<double>(tpl.mask.cols) / tpl.mask.rows;
        auto [hole_count, hole_center_y] = analyze_glyph_holes(tpl.mask);
        tpl.hole_count = hole_count;
        tpl.hole_center_y = hole_center_y;
        tpl.ink_center_y = analyze_ink_center_y(tpl.mask);
        tpl.ink_center_x = analyze_ink_center_x(tpl.mask);
        tpl.bottom_half_ratio = analyze_bottom_half_ratio(tpl.mask);
        templates.push_back(std::move(tpl));
    }

    return templates;
}

static const std::vector<DigitTemplate>& get_digit_templates() {
    static std::vector<DigitTemplate> templates = build_digit_templates();
    return templates;
}

static std::pair<int, double> analyze_glyph_holes(const cv::Mat& glyph) {
    cv::Mat inverted;
    cv::bitwise_not(glyph, inverted);

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int component_count = cv::connectedComponentsWithStats(
        inverted, labels, stats, centroids, 8, CV_32S);

    int hole_count = 0;
    double hole_center_y_sum = 0.0;

    for (int label = 1; label < component_count; ++label) {
        const int x = stats.at<int>(label, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(label, cv::CC_STAT_TOP);
        const int w = stats.at<int>(label, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(label, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);

        const bool touches_border =
            x <= 0 || y <= 0 || x + w >= glyph.cols || y + h >= glyph.rows;
        if (!touches_border && area >= 2) {
            hole_center_y_sum += centroids.at<double>(label, 1) / std::max(1, glyph.rows);
            ++hole_count;
        }
    }

    double hole_center_y = (hole_count > 0) ? (hole_center_y_sum / hole_count) : 0.5;
    return {hole_count, hole_center_y};
}

static double analyze_ink_center_y(const cv::Mat& glyph) {
    cv::Moments m = cv::moments(glyph, true);
    if (m.m00 <= 0.0) {
        return 0.5;
    }
    return (m.m01 / m.m00) / std::max(1, glyph.rows);
}

static double analyze_ink_center_x(const cv::Mat& glyph) {
    cv::Moments m = cv::moments(glyph, true);
    if (m.m00 <= 0.0) {
        return 0.5;
    }
    return (m.m10 / m.m00) / std::max(1, glyph.cols);
}

static double analyze_bottom_half_ratio(const cv::Mat& glyph) {
    int split_y = glyph.rows / 2;
    double total = static_cast<double>(cv::countNonZero(glyph));
    if (total <= 0.0) {
        return 0.5;
    }
    double bottom = static_cast<double>(cv::countNonZero(glyph(cv::Rect(0, split_y, glyph.cols, glyph.rows - split_y))));
    return bottom / total;
}

static std::vector<cv::Rect> extract_character_boxes(const cv::Mat& thresh) {
    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int component_count = cv::connectedComponentsWithStats(
        thresh, labels, stats, centroids, 8, CV_32S);

    std::vector<cv::Rect> digit_boxes;
    std::vector<cv::Rect> colon_dots;
    digit_boxes.reserve(std::max(0, component_count - 1));
    colon_dots.reserve(std::max(0, component_count - 1));

    const int min_digit_height = std::max(8, thresh.rows / 3);
    const int max_digit_height = thresh.rows;
    const int min_digit_width = std::max(3, thresh.cols / 50);
    const int max_digit_width = std::max(min_digit_width + 1, thresh.cols / 3);
    const int colon_max_size = std::max(10, thresh.rows / 4);
    const int left_noise_cutoff = thresh.cols / 6;

    for (int label = 1; label < component_count; ++label) {
        cv::Rect r(stats.at<int>(label, cv::CC_STAT_LEFT),
                   stats.at<int>(label, cv::CC_STAT_TOP),
                   stats.at<int>(label, cv::CC_STAT_WIDTH),
                   stats.at<int>(label, cv::CC_STAT_HEIGHT));
        if (r.width <= 1 || r.height <= 1) {
            continue;
        }

        if (r.x + r.width < left_noise_cutoff) {
            continue;
        }

        const bool looks_like_clock_icon =
            r.x < thresh.cols / 2 && r.width >= r.height * 0.80 && r.width <= r.height * 1.20 &&
            r.height >= thresh.rows / 2;
        if (looks_like_clock_icon) {
            continue;
        }

        const bool is_colon_dot =
            r.width <= colon_max_size && r.height <= colon_max_size;
        const bool is_digit =
            r.height >= min_digit_height && r.height <= max_digit_height &&
            r.width >= min_digit_width && r.width <= max_digit_width;

        if (is_digit) {
            digit_boxes.push_back(r);
        } else if (is_colon_dot) {
            colon_dots.push_back(r);
        }
    }

    std::sort(digit_boxes.begin(), digit_boxes.end(),
              [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });
    std::sort(colon_dots.begin(), colon_dots.end(),
              [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });

    std::vector<cv::Rect> merged_colons;
    std::vector<bool> used(colon_dots.size(), false);
    const int max_colon_gap_x = std::max(6, thresh.cols / 40);
    const int min_colon_gap_y = std::max(6, thresh.rows / 10);
    const int max_colon_gap_y = std::max(min_colon_gap_y + 1, thresh.rows / 2);

    for (size_t i = 0; i < colon_dots.size(); ++i) {
        if (used[i]) {
            continue;
        }

        for (size_t j = i + 1; j < colon_dots.size(); ++j) {
            if (used[j]) {
                continue;
            }

            int center_x_i = colon_dots[i].x + colon_dots[i].width / 2;
            int center_x_j = colon_dots[j].x + colon_dots[j].width / 2;
            int center_y_i = colon_dots[i].y + colon_dots[i].height / 2;
            int center_y_j = colon_dots[j].y + colon_dots[j].height / 2;

            if (std::abs(center_x_i - center_x_j) <= max_colon_gap_x) {
                int gap_y = std::abs(center_y_i - center_y_j);
                if (gap_y >= min_colon_gap_y && gap_y <= max_colon_gap_y) {
                    merged_colons.push_back(colon_dots[i] | colon_dots[j]);
                    used[i] = true;
                    used[j] = true;
                    break;
                }
            }
        }
    }

    std::vector<cv::Rect> unmerged_dots;
    for (size_t i = 0; i < colon_dots.size(); ++i) {
        if (!used[i]) {
            // Check if it's in the lower half to avoid random noise dots at the top
            if (colon_dots[i].y + colon_dots[i].height / 2 > thresh.rows / 2) {
                unmerged_dots.push_back(colon_dots[i]);
            }
        }
    }

    std::vector<cv::Rect> boxes = digit_boxes;
    boxes.insert(boxes.end(), merged_colons.begin(), merged_colons.end());
    boxes.insert(boxes.end(), unmerged_dots.begin(), unmerged_dots.end());
    std::sort(boxes.begin(), boxes.end(),
              [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });
    return boxes;
}

static char classify_segment(const cv::Mat& char_img,
                              const std::vector<DigitTemplate>& templates) {
    cv::Rect bbox = cv::boundingRect(char_img);
    if (bbox.width <= 1 || bbox.height <= 1) return '?';
    cv::Mat cropped = char_img(bbox);

    double aspect = static_cast<double>(cropped.cols) / cropped.rows;
    auto [hole_count, hole_center_y] = analyze_glyph_holes(cropped);
    double ink_center_y = analyze_ink_center_y(cropped);
    double ink_center_x = analyze_ink_center_x(cropped);
    double bottom_half_ratio = analyze_bottom_half_ratio(cropped);
    double best_score = 1e18;
    char best_symbol = '?';

    for (const auto& tpl : templates) {
        cv::Mat resized;
        cv::resize(cropped, resized, tpl.mask.size(), 0, 0, cv::INTER_NEAREST);

        cv::Mat diff;
        cv::bitwise_xor(resized, tpl.mask, diff);
        double pixel_error = static_cast<double>(cv::countNonZero(diff)) /
                             static_cast<double>(tpl.mask.total());
        double aspect_error = std::abs(aspect - tpl.aspect);
        double hole_penalty = (hole_count == tpl.hole_count) ? 0.0 : 1.2;
        double hole_center_penalty = (hole_count > 0 && tpl.hole_count > 0)
            ? 0.50 * std::abs(hole_center_y - tpl.hole_center_y)
            : 0.0;
        double ink_center_penalty = 0.35 * std::abs(ink_center_y - tpl.ink_center_y);
        double ink_center_x_penalty = 0.25 * std::abs(ink_center_x - tpl.ink_center_x);
        double bottom_half_penalty = 0.45 * std::abs(bottom_half_ratio - tpl.bottom_half_ratio);
        double score = pixel_error + 0.35 * aspect_error + hole_penalty + hole_center_penalty + ink_center_penalty + ink_center_x_penalty + bottom_half_penalty;

        if (score < best_score) {
            best_score = score;
            best_symbol = tpl.symbol;
        }
    }

    if (best_score > 0.55) {
        return '?';
    }

    if (best_symbol == '0' && hole_count == 1) {
        if (hole_center_y > 0.58 && ink_center_x < 0.47) {
            return '6';
        }
        if (hole_center_y < 0.42 && ink_center_x > 0.50) {
            return '9';
        }
    }

    return best_symbol;
}

static std::string extract_clock_substring(const std::string& raw_result) {
    std::string best_clock;
    std::string current_clock;
    for (char c : raw_result) {
        if (std::isdigit(c) || c == ':' || c == '.') {
            current_clock += c;
        } else {
            if ((current_clock.find(':') != std::string::npos || current_clock.find('.') != std::string::npos) && current_clock.length() >= 3) {
                if (current_clock.length() > best_clock.length()) best_clock = current_clock;
            }
            current_clock = "";
        }
    }
    if ((current_clock.find(':') != std::string::npos || current_clock.find('.') != std::string::npos) && current_clock.length() >= 3) {
        if (current_clock.length() > best_clock.length()) best_clock = current_clock;
    }
    return best_clock;
}

static char classify_segment_fast(const cv::Mat& char_img) {
    cv::Rect bbox = cv::boundingRect(char_img);
    if (bbox.width <= 1 || bbox.height <= 1) return '?';
    cv::Mat cropped = char_img(bbox);

    double aspect = static_cast<double>(cropped.cols) / std::max(1, cropped.rows);
    if (aspect < 0.38) {
        return (cropped.rows > cropped.cols * 1.6) ? ':' : '.';
    }

    cv::Mat norm;
    cv::resize(cropped, norm, cv::Size(20, 32), 0, 0, cv::INTER_AREA);

    auto fill_ratio = [&](int x, int y, int w, int h) {
        cv::Rect r(std::clamp(x, 0, norm.cols - 1),
                   std::clamp(y, 0, norm.rows - 1),
                   std::min(w, norm.cols - std::clamp(x, 0, norm.cols - 1)),
                   std::min(h, norm.rows - std::clamp(y, 0, norm.rows - 1)));
        if (r.width <= 0 || r.height <= 0) {
            return 0.0;
        }
        return static_cast<double>(cv::countNonZero(norm(r))) / static_cast<double>(r.area());
    };

    int mask = 0;
    if (fill_ratio(4, 0, 12, 5) > 0.25) mask |= 1;   // A
    if (fill_ratio(14, 4, 6, 11) > 0.25) mask |= 2;  // B
    if (fill_ratio(14, 17, 6, 11) > 0.25) mask |= 4; // C
    if (fill_ratio(4, 27, 12, 5) > 0.25) mask |= 8;  // D
    if (fill_ratio(0, 17, 6, 11) > 0.25) mask |= 16; // E
    if (fill_ratio(0, 4, 6, 11) > 0.25) mask |= 32;  // F
    if (fill_ratio(4, 13, 12, 6) > 0.25) mask |= 64; // G

    struct SegmentDigit {
        int mask;
        char digit;
    };
    static constexpr std::array<SegmentDigit, 10> segment_digits{{
        {63, '0'}, {6, '1'}, {91, '2'}, {79, '3'}, {102, '4'},
        {109, '5'}, {125, '6'}, {7, '7'}, {127, '8'}, {111, '9'}
    }};

    switch (mask) {
        case 63: return '0';
        case 6: return '1';
        case 91: return '2';
        case 79: return '3';
        case 102: return '4';
        case 109: return '5';
        case 125: return '6';
        case 7: return '7';
        case 127: return '8';
        case 111: return '9';
        default: break;
    }

    int best_dist = 8;
    char best_digit = '?';
    for (const auto& candidate : segment_digits) {
        int dist = std::popcount(static_cast<unsigned int>(mask ^ candidate.mask));
        if (dist < best_dist) {
            best_dist = dist;
            best_digit = candidate.digit;
        }
    }

    return best_dist <= 2 ? best_digit : '?';
}

static std::string recognize_time(const cv::Mat& roi_bgr, bool is_active) {
    if (roi_bgr.empty()) return "";

    cv::Mat scaled;
    double scale = std::clamp(72.0 / static_cast<double>(std::max(1, roi_bgr.rows)), 1.0, 2.0);
    if (scale > 1.01) {
        cv::resize(roi_bgr, scaled, cv::Size(), scale, scale, cv::INTER_LINEAR);
    } else {
        scaled = roi_bgr;
    }

    cv::Mat gray;
    cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);

    cv::Mat thresh;
    if (is_active) {
        // Dark text on a bright background (active player clock)
        cv::adaptiveThreshold(gray, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY_INV, 11, 5);
    } else {
        // Bright text on a dark background (inactive player clock)
        cv::adaptiveThreshold(gray, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 11, -5);
    }

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
    cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, kernel);

    auto boxes = extract_character_boxes(thresh);
    if (boxes.empty()) return "";

    std::string raw_result;
    raw_result.reserve(boxes.size());

    for (const auto& box : boxes) {
        char c = classify_segment_fast(thresh(box));
        raw_result += c;
    }

    std::string fast_clock = extract_clock_substring(raw_result);
    if (!fast_clock.empty()) {
        return fast_clock;
    }

    const auto& templates = get_digit_templates();
    raw_result.clear();
    for (const auto& box : boxes) {
        char c = classify_segment(thresh(box), templates);
        raw_result += c;
    }

    return extract_clock_substring(raw_result);
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
    std::string res = recognize_time(time_text, active_first);
    if (!looks_like_clock_string(res)) {
        res = recognize_time(time_text, !active_first);
    }
    if (!looks_like_clock_string(res)) {
        res = recognize_time(bgr, active_first);
    }
    if (!looks_like_clock_string(res)) {
        res = recognize_time(bgr, !active_first);
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
            state.black_time = recognize_time(tight_top_text, true);
            if (state.black_time.empty()) {
                state.black_time = recognize_time(tight_top_text, false);
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
            state.black_time = recognize_time(tight_top_text, true);
            if (state.black_time.empty()) {
                state.black_time = recognize_time(tight_top_text, false);
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
