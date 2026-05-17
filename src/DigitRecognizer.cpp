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
namespace DigitRecognizer {

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
        if (used[i]) continue;
        for (size_t j = i + 1; j < colon_dots.size(); ++j) {
            if (used[j]) continue;

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

static int count_glyph_holes_binary(const cv::Mat& glyph) {
    cv::Mat inverted;
    cv::bitwise_not(glyph, inverted);

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    int component_count = cv::connectedComponentsWithStats(
        inverted, labels, stats, centroids, 8, CV_32S);

    int holes = 0;
    for (int label = 1; label < component_count; ++label) {
        const int x = stats.at<int>(label, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(label, cv::CC_STAT_TOP);
        const int w = stats.at<int>(label, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(label, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (x > 0 && y > 0 && x + w < glyph.cols && y + h < glyph.rows && area >= 2) {
            ++holes;
        }
    }
    return holes;
}

static char classify_clock_glyph_shape(const cv::Mat& glyph) {
    cv::Rect bbox = cv::boundingRect(glyph);
    if (bbox.width <= 1 || bbox.height <= 1) return '?';

    cv::Mat cropped = glyph(bbox);
    int holes = count_glyph_holes_binary(cropped);
    if (bbox.width <= 9 || static_cast<double>(bbox.width) / std::max(1, bbox.height) < 0.55) {
        return '1';
    }
    if (holes >= 2) {
        return '8';
    }

    cv::Mat norm;
    cv::resize(cropped, norm, cv::Size(20, 32), 0, 0, cv::INTER_NEAREST);

    auto fill = [&](int x, int y, int w, int h) {
        cv::Rect r(std::clamp(x, 0, norm.cols - 1),
                   std::clamp(y, 0, norm.rows - 1),
                   std::min(w, norm.cols - std::clamp(x, 0, norm.cols - 1)),
                   std::min(h, norm.rows - std::clamp(y, 0, norm.rows - 1)));
        if (r.width <= 0 || r.height <= 0) return 0.0;
        return static_cast<double>(cv::countNonZero(norm(r))) / static_cast<double>(r.area());
    };

    double tl = fill(0, 0, 7, 16);
    double tr = fill(13, 0, 7, 16);
    double ml = fill(0, 10, 10, 12);
    double bl = fill(0, 16, 7, 16);
    double br = fill(13, 16, 7, 16);
    double top = fill(4, 0, 12, 6);
    double mid = fill(4, 12, 12, 8);
    double bot = fill(4, 26, 12, 6);

    if (holes == 1) {
        if (ml > 0.60 && bl > 0.55 && mid > 0.40) return '6';
        if (mid < 0.38) return '0';
        if (top < 0.55 && bot < 0.45) return '4';
        if (tl > 0.58 && bl > 0.70 && tr < 0.52) return '6';
        if (bl < 0.55 && mid > 0.55) return '9';
        return (tl > tr) ? '6' : '0';
    }

    if (top > 0.85 && bot < 0.70 && bl < 0.20) return '7';
    if (ml < 0.12 && bot > 0.80) return '2';
    if (tl > 0.50 && top > 0.85 && mid > 0.58 && br > 0.65) return '5';
    if (tl > 0.55 && tr < 0.55 && mid > 0.65 && bot > 0.85 && br > 0.70) return '5';
    return '3';
}

static std::string recognize_clock_by_components(const cv::Mat& roi_bgr) {
    if (roi_bgr.empty()) return "";

    cv::Mat gray;
    cv::cvtColor(roi_bgr, gray, cv::COLOR_BGR2GRAY);

    std::vector<cv::Mat> candidates;
    cv::Mat otsu_binary;
    cv::threshold(gray, otsu_binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    candidates.push_back(otsu_binary);

    cv::Mat otsu_inverse;
    cv::threshold(gray, otsu_inverse, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    candidates.push_back(otsu_inverse);

    for (const cv::Mat& thresh : candidates) {
        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        int component_count = cv::connectedComponentsWithStats(
            thresh, labels, stats, centroids, 8, CV_32S);

        std::vector<cv::Rect> digit_boxes;
        for (int label = 1; label < component_count; ++label) {
            cv::Rect r(stats.at<int>(label, cv::CC_STAT_LEFT),
                       stats.at<int>(label, cv::CC_STAT_TOP),
                       stats.at<int>(label, cv::CC_STAT_WIDTH),
                       stats.at<int>(label, cv::CC_STAT_HEIGHT));
            int area = stats.at<int>(label, cv::CC_STAT_AREA);
            if (r.width <= 1 || r.height <= 1 || area < 20) continue;

            bool looks_like_clock_icon =
                r.x < thresh.cols / 2 &&
                r.width >= r.height * 0.80 && r.width <= r.height * 1.20 &&
                r.height >= thresh.rows / 2;
            if (looks_like_clock_icon) continue;

            if (r.height >= std::max(12, thresh.rows / 3) &&
                r.width >= 4 &&
                r.width <= std::max(16, thresh.cols / 4)) {
                int max_single_digit_width = std::max(18, static_cast<int>(std::round(r.height * 0.75)));
                if (r.width > max_single_digit_width) {
                    int split_count = std::clamp(
                        static_cast<int>(std::round(static_cast<double>(r.width) / std::max(1, max_single_digit_width))),
                        2, 3);
                    int x = r.x;
                    for (int part = 0; part < split_count; ++part) {
                        int next_x = r.x + static_cast<int>(std::round((part + 1) * r.width / static_cast<double>(split_count)));
                        digit_boxes.emplace_back(x, r.y, next_x - x, r.height);
                        x = next_x;
                    }
                } else {
                    digit_boxes.push_back(r);
                }
            }
        }

        std::sort(digit_boxes.begin(), digit_boxes.end(),
                  [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });

        if (digit_boxes.size() > 5) {
            digit_boxes.erase(digit_boxes.begin(), digit_boxes.end() - 5);
        }
        if (digit_boxes.size() < 3 || digit_boxes.size() > 5) {
            continue;
        }

        std::string digits;
        digits.reserve(digit_boxes.size());
        bool all_ok = true;
        for (const cv::Rect& box : digit_boxes) {
            char c = classify_clock_glyph_shape(thresh(box));
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_ok = false;
                break;
            }
            digits += c;
        }
        if (all_ok) {
            if (digits.size() == 5) {
                return std::string(1, digits[0]) + ":" + digits.substr(1, 2) + ":" + digits.substr(3, 2);
            }
            if (digits.size() == 4) {
                return digits.substr(0, 2) + ":" + digits.substr(2, 2);
            }
            return std::string(1, digits[0]) + ":" + digits.substr(1, 2);
        }
    }

    return "";
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

std::string recognize_time(const cv::Mat& roi_bgr, bool is_active) {
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

    std::string component_clock = recognize_clock_by_components(scaled);
    if (!component_clock.empty()) {
        return component_clock;
    }

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

} // namespace DigitRecognizer
} // namespace cta
