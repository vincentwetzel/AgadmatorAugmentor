// Extracted from cpp directory
#include "ClockRecognizer.h"
#include "BoardLocalizer.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <array>
#include <cctype>
#include <string>
#include <tuple>
#include <vector>

namespace aa {

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
        std::vector<cv::Point> pts;
        cv::findNonZero(binary, pts);
        if (pts.empty()) {
            continue;
        }

        cv::Rect bbox = cv::boundingRect(pts);
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
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(glyph.clone(), contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);

    int hole_count = 0;
    double hole_center_y_sum = 0.0;
    for (size_t i = 0; i < hierarchy.size(); ++i) {
        if (hierarchy[i][3] >= 0) {
            cv::Rect r = cv::boundingRect(contours[i]);
            hole_center_y_sum += (r.y + r.height * 0.5) / std::max(1, glyph.rows);
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

static std::vector<std::tuple<int, int, cv::Mat>> segment_characters(const cv::Mat& thresh) {
    std::vector<std::tuple<int, int, cv::Mat>> segments;
    cv::Mat proj;
    cv::reduce(thresh, proj, 0, cv::REDUCE_SUM, CV_32S);
    int w = thresh.cols;
    const int min_col_sum = 0;

    int seg_start = -1;
    for (int x = 0; x < w; ++x) {
        int col_sum = proj.at<int>(0, x);
        if (col_sum > min_col_sum) {
            if (seg_start < 0) seg_start = x;
        } else if (seg_start >= 0) {
            int seg_end = x;
            if (seg_end - seg_start >= 1) {
                segments.emplace_back(seg_start, seg_end,
                    thresh(cv::Rect(seg_start, 0, seg_end - seg_start, thresh.rows)).clone());
            }
            seg_start = -1;
        }
    }
    if (seg_start >= 0 && w - seg_start >= 1) {
        segments.emplace_back(seg_start, w,
            thresh(cv::Rect(seg_start, 0, w - seg_start, thresh.rows)).clone());
    }
    return segments;
}

static std::vector<cv::Rect> extract_character_boxes(const cv::Mat& thresh) {
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<cv::Rect> digit_boxes;
    std::vector<cv::Rect> colon_dots;
    digit_boxes.reserve(contours.size());
    colon_dots.reserve(contours.size());

    const int min_digit_height = std::max(8, thresh.rows / 3);
    const int max_digit_height = thresh.rows;
    const int min_digit_width = std::max(3, thresh.cols / 50);
    const int max_digit_width = std::max(min_digit_width + 1, thresh.cols / 3);
    const int colon_max_size = std::max(10, thresh.rows / 4);
    const int left_noise_cutoff = thresh.cols / 6;

    for (const auto& cnt : contours) {
        cv::Rect r = cv::boundingRect(cnt);
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

static std::string recognize_time(const cv::Mat& roi_bgr, bool is_active) {
    if (roi_bgr.empty()) return "";

    cv::Mat scaled;
    cv::resize(roi_bgr, scaled, cv::Size(), 4.0, 4.0, cv::INTER_CUBIC);

    cv::Mat gray;
    cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);

    cv::Mat thresh;
    if (is_active) {
        cv::threshold(gray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    } else {
        cv::threshold(gray, thresh, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    }

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
    cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, kernel);
    cv::morphologyEx(thresh, thresh, cv::MORPH_OPEN, kernel);

    auto boxes = extract_character_boxes(thresh);
    if (boxes.empty()) return "";

    const auto& templates = get_digit_templates();
    std::string raw_result;
    raw_result.reserve(boxes.size());

    for (const auto& box : boxes) {
        char c = classify_segment(thresh(box), templates);
        raw_result += c;
    }

    // Extract the longest valid clock substring (e.g., "1:05") to ignore surrounding player names/avatars
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

// ── Clock extraction ─────────────────────────────────────────────────────────

ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo,
                          ClockCache* cache) {
    // The chess.com clocks are right-aligned to the board, so keep the ROI
    // tight around the pill instead of sweeping a broad strip that includes UI text.
    int roi_x1 = std::max(0, static_cast<int>(geo.bx + geo.bw * 0.76));
    int roi_x2 = std::min(img_bgr.cols, static_cast<int>(geo.bx + geo.bw));

    int top_roi_y1 = std::max(0, static_cast<int>(geo.by - geo.sq_h * 0.40));
    int top_roi_y2 = std::max(top_roi_y1 + 1, static_cast<int>(geo.by - geo.sq_h * 0.03));
    int bot_roi_y1 = std::min(img_bgr.rows - 1, static_cast<int>(geo.by + geo.bh + geo.sq_h * 0.07));
    int bot_roi_y2 = std::min(img_bgr.rows, static_cast<int>(geo.by + geo.bh + geo.sq_h * 0.40));

    if (roi_x2 <= roi_x1 || top_roi_y2 <= top_roi_y1 || bot_roi_y2 <= bot_roi_y1) {
        return {};
    }

    cv::Mat top_bgr = img_bgr(cv::Rect(roi_x1, top_roi_y1, roi_x2 - roi_x1, top_roi_y2 - top_roi_y1));
    cv::Mat bot_bgr = img_bgr(cv::Rect(roi_x1, bot_roi_y1, roi_x2 - roi_x1, bot_roi_y2 - bot_roi_y1));

    auto count_white = [](const cv::Mat& roi) {
        cv::Mat gray;
        cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
        cv::Mat mask;
        cv::threshold(gray, mask, 200, 255, cv::THRESH_BINARY);
        return cv::countNonZero(mask);
    };

    int top_white = count_white(top_bgr);
    int bot_white = count_white(bot_bgr);

    ClockState state;
    if (top_white < 50 && bot_white < 50) {
        state.active_player = "";
    } else {
        state.active_player = (bot_white > top_white) ? "white" : "black";
    }

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
        // Try both dark-on-light and light-on-dark thresholding to support any overlay theme.
        auto robust_recognize = [](const cv::Mat& bgr) {
            std::string res = recognize_time(bgr, false);
            if (res.length() < 3 || (res.find(':') == std::string::npos && res.find('.') == std::string::npos)) {
                res = recognize_time(bgr, true);
            }
            return res;
        };

        auto crop_right_aligned_text = [](const cv::Mat& bgr) {
            int x1 = std::clamp(static_cast<int>(bgr.cols * 0.34), 0, std::max(0, bgr.cols - 1));
            int width = bgr.cols - x1;
            if (width <= 0) {
                return bgr.clone();
            }
            return bgr(cv::Rect(x1, 0, width, bgr.rows)).clone();
        };

        state.white_time = robust_recognize(bot_bgr);
        if (state.white_time.empty()) {
            state.white_time = robust_recognize(crop_right_aligned_text(bot_bgr));
        }

        state.black_time = robust_recognize(top_bgr);
        if (state.black_time.empty()) {
            state.black_time = robust_recognize(crop_right_aligned_text(top_bgr));
        }

        if (state.black_time.empty() && state.active_player == "black") {
            auto tight_top_text = top_bgr(cv::Rect(
                std::clamp(static_cast<int>(top_bgr.cols * 0.40), 0, std::max(0, top_bgr.cols - 1)),
                0,
                top_bgr.cols - std::clamp(static_cast<int>(top_bgr.cols * 0.40), 0, std::max(0, top_bgr.cols - 1)),
                top_bgr.rows)).clone();
            state.black_time = recognize_time(tight_top_text, true);
            if (state.black_time.empty()) {
                state.black_time = recognize_time(tight_top_text, false);
            }
        }

        state.ocr_skipped = false;

        if (cache) {
            cache->top_gray = top_gray.clone();
            cache->bot_gray = bot_gray.clone();
            cache->white_time = state.white_time;
            cache->black_time = state.black_time;
            cache->valid = true;
        }
    }

    return state;
}

} // namespace aa
