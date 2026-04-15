// Extracted from cpp directory
#include "ClockRecognizer.h"
#include "BoardLocalizer.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <array>
#include <string>
#include <vector>

namespace aa {

// ── Hu Moments Digit Recognizer ──────────────────────────────────────────────
// Zero-dependency OCR for chess clock digits (0-9, ":").
// Uses pre-computed 7-segment display templates and nearest-neighbor
// classification on 7 log-transformed Hu moments.

struct DigitTemplate {
    std::array<double, 7> hu;
    double area;
    double aspect;
};

static std::vector<DigitTemplate> build_digit_templates() {
    std::vector<DigitTemplate> templates(11);

    auto make_template = [](const std::vector<std::string>& rows, DigitTemplate& tpl) {
        int h = static_cast<int>(rows.size());
        int w = h > 0 ? static_cast<int>(rows[0].size()) : 0;
        cv::Mat img(h, w, CV_8UC1, cv::Scalar(0));
        for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w && c < static_cast<int>(rows[r].size()); ++c) {
                if (rows[r][c] == '#') img.at<uchar>(r, c) = 255;
            }
        }
        cv::Moments m = cv::moments(img, true);
        cv::HuMoments(m, tpl.hu.data());
        for (int i = 0; i < 7; ++i) {
            tpl.hu[i] = -std::copysign(std::log10(std::abs(tpl.hu[i]) + 1e-10), tpl.hu[i]);
        }
        tpl.area = static_cast<double>(cv::countNonZero(img)) / (w * h + 1);
        tpl.aspect = (w > 0 && h > 0) ? static_cast<double>(w) / h : 1.0;
    };

    // 7-segment digital clock style digit templates (8 rows x 5 cols)
    make_template({" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### ", "     "}, templates[0]);
    make_template({"  #  ", " ##  ", "# #  ", "  #  ", "  #  ", "  #  ", "#####", "     "}, templates[1]);
    make_template({" ### ", "#   #", "    #", " ### ", "#    ", "#    ", "#####", "     "}, templates[2]);
    make_template({" ### ", "#   #", "    #", " ### ", "    #", "#   #", " ### ", "     "}, templates[3]);
    make_template({"#   #", "#   #", "#   #", " #####", "    #", "    #", "    #", "     "}, templates[4]);
    make_template({"#####", "#    ", "#    ", "#####", "    #", "#   #", " ### ", "     "}, templates[5]);
    make_template({" ### ", "#    ", "#    ", "#####", "#   #", "#   #", " ### ", "     "}, templates[6]);
    make_template({"#####", "#   #", "   # ", "  #  ", " #   ", " #   ", " #   ", "     "}, templates[7]);
    make_template({" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### ", "     "}, templates[8]);
    make_template({" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### ", "     "}, templates[9]);
    make_template({"     ", "  #  ", "     ", "     ", "  #  ", "     ", "     ", "     "}, templates[10]);

    return templates;
}

static const std::vector<DigitTemplate>& get_digit_templates() {
    static std::vector<DigitTemplate> templates = build_digit_templates();
    return templates;
}

static std::vector<std::tuple<int, int, cv::Mat>> segment_characters(const cv::Mat& thresh) {
    std::vector<std::tuple<int, int, cv::Mat>> segments;
    cv::Mat proj;
    cv::reduce(thresh, proj, 0, cv::REDUCE_SUM, CV_32S);
    int w = thresh.cols;
    const int min_col_sum = 3;

    int seg_start = -1;
    for (int x = 0; x < w; ++x) {
        int col_sum = proj.at<int>(0, x);
        if (col_sum > min_col_sum) {
            if (seg_start < 0) seg_start = x;
        } else if (seg_start >= 0) {
            int seg_end = x;
            if (seg_end - seg_start >= 3) {
                segments.emplace_back(seg_start, seg_end,
                    thresh(cv::Rect(seg_start, 0, seg_end - seg_start, thresh.rows)).clone());
            }
            seg_start = -1;
        }
    }
    if (seg_start >= 0 && w - seg_start >= 3) {
        segments.emplace_back(seg_start, w,
            thresh(cv::Rect(seg_start, 0, w - seg_start, thresh.rows)).clone());
    }
    return segments;
}

static char classify_segment(const cv::Mat& char_img,
                              const std::vector<DigitTemplate>& templates) {
    cv::Mat resized;
    cv::resize(char_img, resized, cv::Size(5, 8), 0, 0, cv::INTER_LINEAR);

    cv::Moments m = cv::moments(resized, true);
    std::array<double, 7> hu;
    cv::HuMoments(m, hu.data());
    for (int i = 0; i < 7; ++i) {
        hu[i] = -std::copysign(std::log10(std::abs(hu[i]) + 1e-10), hu[i]);
    }

    double area = static_cast<double>(cv::countNonZero(resized)) / 40.0;
    double aspect = 5.0 / 8.0;

    double best_score = 1e18;
    int best_idx = -1;

    for (size_t i = 0; i < templates.size(); ++i) {
        double dist = 0.0;
        for (int j = 0; j < 7; ++j) {
            double d = hu[j] - templates[i].hu[j];
            dist += d * d;
        }
        dist += 5.0 * std::abs(area - templates[i].area);
        dist += 3.0 * std::abs(aspect - templates[i].aspect);
        if (dist < best_score) {
            best_score = dist;
            best_idx = static_cast<int>(i);
        }
    }

    if (best_score > 15.0) return '?';
    if (best_idx < 10) return static_cast<char>('0' + best_idx);
    return ':';
}

static std::string recognize_time(const cv::Mat& roi_bgr, bool is_active) {
    if (roi_bgr.empty()) return "";

    cv::Mat scaled;
    cv::resize(roi_bgr, scaled, cv::Size(), 3.0, 3.0, cv::INTER_CUBIC);

    cv::Mat gray;
    cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);

    cv::Mat thresh;
    if (is_active) {
        cv::adaptiveThreshold(gray, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY, 21, -10);
        thresh = ~thresh;
    } else {
        cv::adaptiveThreshold(gray, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY, 21, 5);
    }

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
    cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, kernel);

    std::vector<cv::Point> pts;
    cv::findNonZero(thresh, pts);
    if (pts.empty()) return "";

    cv::Rect bbox = cv::boundingRect(pts);
    bbox.x = std::max(0, bbox.x - 2);
    bbox.y = std::max(0, bbox.y - 1);
    bbox.width = std::min(bbox.width + 4, thresh.cols - bbox.x);
    bbox.height = std::min(bbox.height + 2, thresh.rows - bbox.y);

    cv::Mat text_region = thresh(bbox);
    auto segments = segment_characters(text_region);
    if (segments.empty()) return "";

    const auto& templates = get_digit_templates();
    std::string result;
    result.reserve(segments.size());

    for (const auto& [x1, x2, img] : segments) {
        char c = classify_segment(img, templates);
        result += c;
    }

    int colons = 0, digits = 0;
    for (char c : result) {
        if (c == ':') ++colons;
        else if (std::isdigit(c)) ++digits;
    }
    if (colons < 1 || digits < 3) return "";

    return result;
}

// ── Clock extraction ─────────────────────────────────────────────────────────

ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo,
                          ClockCache* cache) {
    int roi_x1 = std::max(0, static_cast<int>(geo.bx + geo.bw - geo.sq_w * 1.18));
    int roi_x2 = std::min(img_bgr.cols, static_cast<int>(geo.bx + geo.bw + geo.sq_w * 0.05));

    int top_roi_y1 = std::max(0, static_cast<int>(geo.by - geo.sq_h * 0.40));
    int top_roi_y2 = std::max(0, static_cast<int>(geo.by - geo.sq_h * 0.08));
    int bot_roi_y1 = std::min(img_bgr.rows, static_cast<int>(geo.by + geo.bh + geo.sq_h * 0.08));
    int bot_roi_y2 = std::min(img_bgr.rows, static_cast<int>(geo.by + geo.bh + geo.sq_h * 0.40));

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
        state.white_time = recognize_time(bot_bgr, state.active_player == "white");
        state.black_time = recognize_time(top_bgr, state.active_player == "black");
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
