#include "UIDetectors.h"
#include "BoardLocalizer.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aa {

// ── Helpers ──────────────────────────────────────────────────────────────────

static double mean_corner_yellowness(const cv::Mat& board_bgr, int row, int col,
                                      double sq_h, double sq_w) {
    int y1 = static_cast<int>(row * sq_h);
    int y2 = static_cast<int>((row + 1) * sq_h);
    int x1 = static_cast<int>(col * sq_w);
    int x2 = static_cast<int>((col + 1) * sq_w);
    int ch = static_cast<int>(sq_h * 0.12);
    int cw = static_cast<int>(sq_w * 0.12);

    // Extract 4 corner patches, compute yellowness: (R + G) / 2.0 - B
    double score = 0.0;
    std::vector<cv::Rect> patches = {
        {x1, y1, cw, ch},
        {x2 - cw, y1, cw, ch},
        {x1, y2 - ch, cw, ch},
        {x2 - cw, y2 - ch, cw, ch}
    };

    for (const auto& p : patches) {
        cv::Mat patch;
        board_bgr(p).convertTo(patch, CV_32FC3);
        std::vector<cv::Mat> channels;
        cv::split(patch, channels);
        // channels[0] = B, channels[1] = G, channels[2] = R
        cv::Mat yellowness = (channels[2] + channels[1]) / 2.0f - channels[0];
        cv::Scalar m = cv::mean(yellowness);
        score += m[0];
    }
    return score / 4.0;
}

static double get_edge_score(const cv::Mat& board_gray, int row, int col,
                             double sq_h, double sq_w) {
    int y1 = static_cast<int>(row * sq_h + sq_h * 0.15);
    int y2 = static_cast<int>((row + 1) * sq_h - sq_h * 0.15);
    int x1 = static_cast<int>(col * sq_w + sq_w * 0.15);
    int x2 = static_cast<int>((col + 1) * sq_w - sq_w * 0.15);

    cv::Mat sq_gray = board_gray(cv::Rect(x1, y1, x2 - x1, y2 - y1));
    cv::Mat blurred;
    cv::GaussianBlur(sq_gray, blurred, cv::Size(3, 3), 0);
    cv::Mat edges;
    cv::Canny(blurred, edges, 50, 150);
    return cv::mean(edges)[0];
}

// ── Yellow square move detection ─────────────────────────────────────────────

std::string extract_move_from_yellow_squares(const cv::Mat& img_bgr,
                                              const cv::Mat& board_template,
                                              const BoardGeometry& geo) {
    cv::Mat board_img = img_bgr(cv::Rect(geo.bx, geo.by, geo.bw, geo.bh));

    // Compute yellowness map
    cv::Mat board_float;
    board_img.convertTo(board_float, CV_32FC3);
    std::vector<cv::Mat> channels;
    cv::split(board_float, channels);
    cv::Mat yellowness_map = (channels[2] + channels[1]) / 2.0f - channels[0];

    // Score all 64 squares
    std::vector<std::pair<double, int>> sq_scores; // (score, square_index 0-63)
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            double score = mean_corner_yellowness(board_img, row, col, geo.sq_h, geo.sq_w);
            sq_scores.emplace_back(score, row * 8 + col);
        }
    }

    // Find top 2 squares
    std::sort(sq_scores.begin(), sq_scores.end(), std::greater<>());
    int idx1 = sq_scores[0].second;
    int idx2 = sq_scores[1].second;

    int row1 = idx1 / 8, col1 = idx1 % 8;
    int row2 = idx2 / 8, col2 = idx2 % 8;

    // Edge detection to determine piece vs empty
    cv::Mat board_gray;
    cv::cvtColor(board_img, board_gray, cv::COLOR_BGR2GRAY);

    double score1 = get_edge_score(board_gray, row1, col1, geo.sq_h, geo.sq_w);
    double score2 = get_edge_score(board_gray, row2, col2, geo.sq_h, geo.sq_w);

    int to_row, to_col, from_row, from_col;
    if (score1 > score2) {
        to_row = row1; to_col = col1; from_row = row2; from_col = col2;
    } else {
        to_row = row2; to_col = col2; from_row = row1; from_col = col1;
    }

    char from_file = 'a' + from_col;
    int from_rank = 8 - from_row;
    char to_file = 'a' + to_col;
    int to_rank = 8 - to_row;

    char uci[5];
    uci[0] = from_file; uci[1] = '0' + from_rank;
    uci[2] = to_file;   uci[3] = '0' + to_rank;
    uci[4] = '\0';
    return std::string(uci);
}

// ── Piece counting ───────────────────────────────────────────────────────────

int count_pieces_in_image(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo) {
    cv::Mat board_img = img_bgr(cv::Rect(geo.bx, geo.by, geo.bw, geo.bh));
    cv::Mat board_gray;
    cv::cvtColor(board_img, board_gray, cv::COLOR_BGR2GRAY);

    int count = 0;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = static_cast<int>(row * geo.sq_h + geo.sq_h * 0.15);
            int y2 = static_cast<int>((row + 1) * geo.sq_h - geo.sq_h * 0.15);
            int x1 = static_cast<int>(col * geo.sq_w + geo.sq_w * 0.15);
            int x2 = static_cast<int>((col + 1) * geo.sq_w - geo.sq_w * 0.15);

            cv::Mat sq_gray = board_gray(cv::Rect(x1, y1, x2 - x1, y2 - y1));
            cv::Mat blurred;
            cv::GaussianBlur(sq_gray, blurred, cv::Size(3, 3), 0);
            cv::Mat edges;
            cv::Canny(blurred, edges, 40, 100);

            if (cv::mean(edges)[0] > 10.0) {
                ++count;
            }
        }
    }
    return count;
}

// ── Red square detection ─────────────────────────────────────────────────────

std::vector<std::string> find_red_squares(const cv::Mat& img_bgr,
                                          const cv::Mat& board_template,
                                          const cv::Mat& red_board_template,
                                          const BoardGeometry& geo) {
    cv::Mat board_img = img_bgr(cv::Rect(geo.bx, geo.by, geo.bw, geo.bh));

    cv::Mat board_float;
    board_img.convertTo(board_float, CV_32FC3);
    std::vector<cv::Mat> channels;
    cv::split(board_float, channels);
    cv::Mat redness_map = channels[2] - (channels[1] + channels[0]) / 2.0f;

    // Compute dynamic threshold
    cv::Mat tpl_float;
    board_template.convertTo(tpl_float, CV_32FC3);
    std::vector<cv::Mat> tpl_ch;
    cv::split(tpl_float, tpl_ch);
    double normal_redness = cv::mean(tpl_ch[2] - (tpl_ch[1] + tpl_ch[0]) / 2.0f)[0];
    double threshold = normal_redness + 35.0;

    if (!red_board_template.empty()) {
        cv::Mat result;
        cv::matchTemplate(red_board_template, board_template, result, cv::TM_CCOEFF_NORMED);
        cv::Point max_loc;
        cv::minMaxLoc(result, nullptr, nullptr, nullptr, &max_loc);

        int rx = max_loc.x, ry = max_loc.y;
        int rh = board_template.rows, rw = board_template.cols;

        if (ry + rh <= red_board_template.rows && rx + rw <= red_board_template.cols) {
            cv::Mat red_cropped = red_board_template(cv::Rect(rx, ry, rw, rh));
            cv::Mat red_float;
            red_cropped.convertTo(red_float, CV_32FC3);
            std::vector<cv::Mat> red_ch;
            cv::split(red_float, red_ch);
            double red_redness = cv::mean(red_ch[2] - (red_ch[1] + red_ch[0]) / 2.0f)[0];
            threshold = (normal_redness + red_redness) / 2.0;
        }
    }

    int ch = static_cast<int>(geo.sq_h * 0.12);
    int cw = static_cast<int>(geo.sq_w * 0.12);

    std::vector<std::string> red_squares;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = static_cast<int>(row * geo.sq_h);
            int y2 = static_cast<int>((row + 1) * geo.sq_h);
            int x1 = static_cast<int>(col * geo.sq_w);
            int x2 = static_cast<int>((col + 1) * geo.sq_w);

            cv::Rect corners[4] = {
                {x1, y1, cw, ch},
                {x2 - cw, y1, cw, ch},
                {x1, y2 - ch, cw, ch},
                {x2 - cw, y2 - ch, cw, ch}
            };

            int red_count = 0;
            for (const auto& c : corners) {
                if (cv::mean(redness_map(c))[0] > threshold) ++red_count;
            }

            if (red_count >= 3) {
                char sq[3];
                sq[0] = 'a' + col;
                sq[1] = '0' + (8 - row);
                sq[2] = '\0';
                red_squares.emplace_back(sq);
            }
        }
    }
    std::sort(red_squares.begin(), red_squares.end());
    return red_squares;
}

// ── Yellow arrow detection ───────────────────────────────────────────────────

std::vector<std::string> find_yellow_arrows(const cv::Mat& img_bgr,
                                            const cv::Mat& board_template,
                                            const BoardGeometry& geo) {
    cv::Mat board_img = img_bgr(cv::Rect(geo.bx, geo.by, geo.bw, geo.bh));

    // HSV masking for saturated yellow/orange
    cv::Mat hsv;
    cv::cvtColor(board_img, hsv, cv::COLOR_BGR2HSV);
    cv::Mat arrow_mask;
    cv::inRange(hsv, cv::Scalar(10, 165, 165), cv::Scalar(40, 255, 255), arrow_mask);

    // Clean up noise
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(arrow_mask, arrow_mask, cv::MORPH_OPEN, kernel);

    // Compute square centers
    struct Center { double cx, cy; std::string name; };
    std::vector<Center> centers(64);
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            int idx = r * 8 + c;
            centers[idx] = {
                (c + 0.5) * geo.sq_w,
                (r + 0.5) * geo.sq_h,
                std::string(1, 'a' + c) + std::to_string(8 - r)
            };
        }
    }

    // Pre-filter active squares
    std::vector<bool> active(64, false);
    for (int i = 0; i < 64; ++i) {
        int r = i / 8, c = i % 8;
        int y1 = static_cast<int>(r * geo.sq_h);
        int y2 = static_cast<int>((r + 1) * geo.sq_h);
        int x1 = static_cast<int>(c * geo.sq_w);
        int x2 = static_cast<int>((c + 1) * geo.sq_w);
        int area = (x2 - x1) * (y2 - y1);
        int nonzero = cv::countNonZero(arrow_mask(cv::Rect(x1, y1, x2 - x1, y2 - y1)));
        active[i] = nonzero > static_cast<int>(area * 0.015);
    }

    // Cast rays between all pairs of active squares
    struct LineCandidate {
        int sq1, sq2;
        int area;
        cv::Mat line_mask;
    };
    std::vector<LineCandidate> candidates;

    for (int i = 0; i < 64; ++i) {
        if (!active[i]) continue;
        for (int j = i + 1; j < 64; ++j) {
            if (!active[j]) continue;

            double dx = centers[j].cx - centers[i].cx;
            double dy = centers[j].cy - centers[i].cy;
            double dist = std::hypot(dx, dy);
            if (dist < geo.sq_w * 0.8) continue;

            cv::Mat line_mask = cv::Mat::zeros(arrow_mask.size(), CV_8UC1);
            cv::line(line_mask,
                     cv::Point(static_cast<int>(centers[i].cx), static_cast<int>(centers[i].cy)),
                     cv::Point(static_cast<int>(centers[j].cx), static_cast<int>(centers[j].cy)),
                     255, static_cast<int>(geo.sq_w * 0.15));

            int line_area = cv::countNonZero(line_mask);
            if (line_area == 0) continue;

            int overlap = cv::countNonZero(arrow_mask & line_mask);
            if (overlap < static_cast<int>(0.60 * line_area)) continue;

            candidates.push_back({i, j, line_area, std::move(line_mask)});
        }
    }

    // Sort by length, process longest first
    std::sort(candidates.begin(), candidates.end(),
              [&](const LineCandidate& a, const LineCandidate& b) {
                  double dxa = centers[a.sq2].cx - centers[a.sq1].cx;
                  double dya = centers[a.sq2].cy - centers[a.sq1].cy;
                  double dxb = centers[b.sq2].cx - centers[b.sq1].cx;
                  double dyb = centers[b.sq2].cy - centers[b.sq1].cy;
                  return (dxa * dxa + dya * dya) > (dxb * dxb + dyb * dyb);
              });

    // Accept non-overlapping lines
    cv::Mat covered_mask = cv::Mat::zeros(arrow_mask.size(), CV_8UC1);
    std::vector<std::pair<int, int>> accepted;

    for (auto& cand : candidates) {
        int already_covered = cv::countNonZero(covered_mask & cand.line_mask);
        if (already_covered < static_cast<int>(0.45 * cand.area)) {
            accepted.emplace_back(cand.sq1, cand.sq2);
            // Draw thick suppression shadow
            cv::Mat thick_mask = cv::Mat::zeros(arrow_mask.size(), CV_8UC1);
            cv::line(thick_mask,
                     cv::Point(static_cast<int>(centers[cand.sq1].cx), static_cast<int>(centers[cand.sq1].cy)),
                     cv::Point(static_cast<int>(centers[cand.sq2].cx), static_cast<int>(centers[cand.sq2].cy)),
                     255, static_cast<int>(geo.sq_w * 1.8));
            cv::bitwise_or(covered_mask, thick_mask, covered_mask);
        }
    }

    // Determine arrow direction via endpoint mass
    std::vector<std::string> arrows;
    for (auto [sq1, sq2] : accepted) {
        cv::Mat mask1 = cv::Mat::zeros(arrow_mask.size(), CV_8UC1);
        cv::Mat mask2 = cv::Mat::zeros(arrow_mask.size(), CV_8UC1);
        int radius = static_cast<int>(geo.sq_w * 0.45);
        cv::circle(mask1, cv::Point(static_cast<int>(centers[sq1].cx), static_cast<int>(centers[sq1].cy)),
                   radius, 255, -1);
        cv::circle(mask2, cv::Point(static_cast<int>(centers[sq2].cx), static_cast<int>(centers[sq2].cy)),
                   radius, 255, -1);

        int count1 = cv::countNonZero(arrow_mask & mask1);
        int count2 = cv::countNonZero(arrow_mask & mask2);

        if (count1 > count2) {
            arrows.push_back(centers[sq1].name + centers[sq2].name);
        } else {
            arrows.push_back(centers[sq2].name + centers[sq1].name);
        }
    }

    std::sort(arrows.begin(), arrows.end());
    arrows.erase(std::unique(arrows.begin(), arrows.end()), arrows.end());
    return arrows;
}

// ── Hover box detection ──────────────────────────────────────────────────────

std::string find_misaligned_piece(const cv::Mat& img_bgr,
                                  const cv::Mat& board_template,
                                  const BoardGeometry& geo) {
    cv::Mat board_img = img_bgr(cv::Rect(geo.bx, geo.by, geo.bw, geo.bh));

    // Isolate white/bright pixels
    cv::Mat white_mask;
    cv::inRange(board_img, cv::Scalar(160, 160, 160), cv::Scalar(255, 255, 255), white_mask);

    int thickness = std::max(3, static_cast<int>(geo.sq_w * 0.08));
    std::string best_square;
    double max_score = -1.0;

    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            int y1 = static_cast<int>(r * geo.sq_h);
            int y2 = static_cast<int>((r + 1) * geo.sq_h);
            int x1 = static_cast<int>(c * geo.sq_w);
            int x2 = static_cast<int>((c + 1) * geo.sq_w);

            cv::Mat top = white_mask(cv::Rect(x1, y1, x2 - x1, thickness));
            cv::Mat bottom = white_mask(cv::Rect(x1, y2 - thickness, x2 - x1, thickness));
            cv::Mat left = white_mask(cv::Rect(x1, y1, thickness, y2 - y1));
            cv::Mat right = white_mask(cv::Rect(x2 - thickness, y1, thickness, y2 - y1));

            int w = x2 - x1, h = y2 - y1;
            double ratios[4];
            // 1D projection via np.max equivalent: collapse axis and count nonzero
            {
                cv::Mat col_max;
                cv::reduce(top, col_max, 0, cv::REDUCE_MAX);
                ratios[0] = static_cast<double>(cv::countNonZero(col_max)) / std::max(1, w);
            }
            {
                cv::Mat col_max;
                cv::reduce(bottom, col_max, 0, cv::REDUCE_MAX);
                ratios[1] = static_cast<double>(cv::countNonZero(col_max)) / std::max(1, w);
            }
            {
                cv::Mat row_max;
                cv::reduce(left, row_max, 1, cv::REDUCE_MAX);
                ratios[2] = static_cast<double>(cv::countNonZero(row_max)) / std::max(1, h);
            }
            {
                cv::Mat row_max;
                cv::reduce(right, row_max, 1, cv::REDUCE_MAX);
                ratios[3] = static_cast<double>(cv::countNonZero(row_max)) / std::max(1, h);
            }

            int visible_edges = 0;
            double sum = 0.0;
            for (double r : ratios) {
                if (r > 0.10) ++visible_edges;
                sum += r;
            }

            if (visible_edges >= 2 && sum > max_score) {
                max_score = sum;
                char sq[3];
                sq[0] = 'a' + c;
                sq[1] = '0' + (8 - r);
                sq[2] = '\0';
                best_square = sq;
            }
        }
    }
    return best_square;
}

// ── Clock extraction ─────────────────────────────────────────────────────────

// Tesseract C API — loaded dynamically via GetProcAddress to avoid /MD linkage
typedef struct TessBaseAPI_TessBaseAPI TessBaseAPI;

// Function pointer types
typedef TessBaseAPI* (*TessCreateFn)(void);
typedef void (*TessDeleteFn)(TessBaseAPI*);
typedef int (*TessInitFn)(TessBaseAPI*, const char*, const char*);
typedef void (*TessSetImageRawFn)(TessBaseAPI*, const unsigned char*, int, int, int, int);
typedef char* (*TessGetTextFn)(TessBaseAPI*);
typedef void (*TessDelTextFn)(char*);
typedef void (*TessSetVarFn)(TessBaseAPI*, const char*, const char*);

// Dynamic loader
struct TessAPI {
    TessCreateFn create;
    TessDeleteFn del;
    TessInitFn init;
    TessSetImageRawFn set_image_raw;
    TessGetTextFn get_text;
    TessDelTextFn del_text;
    TessSetVarFn set_var;
};

static TessAPI load_tesseract() {
    TessAPI api = {};
    const char* tess_paths[] = {
        "E:/vcpkg/installed/x64-windows/bin/tesseract55.dll",
        "tesseract55.dll"
    };
    HMODULE tess_mod = nullptr;
    for (const char* p : tess_paths) {
        tess_mod = LoadLibraryA(p);
        if (tess_mod) break;
    }
    if (!tess_mod) return api;

    api.create = (TessCreateFn)GetProcAddress(tess_mod, "TessBaseAPICreate");
    api.del = (TessDeleteFn)GetProcAddress(tess_mod, "TessBaseAPIDelete");
    api.init = (TessInitFn)GetProcAddress(tess_mod, "TessBaseAPIInit3");
    api.set_image_raw = (TessSetImageRawFn)GetProcAddress(tess_mod, "TessBaseAPISetImage");
    api.get_text = (TessGetTextFn)GetProcAddress(tess_mod, "TessBaseAPIGetUTF8Text");
    api.del_text = (TessDelTextFn)GetProcAddress(tess_mod, "TessDeleteText");
    api.set_var = (TessSetVarFn)GetProcAddress(tess_mod, "TessBaseAPISetVariable");
    return api;
}

// Find time pattern like "1:31:28" or "10:00" in OCR text
static std::string extract_time_string(const std::string& raw) {
    for (size_t i = 0; i + 4 < raw.size(); ++i) {
        size_t start = i;
        int colons = 0;
        size_t j = i;
        bool valid = true;
        while (j < raw.size()) {
            if (raw[j] == ':') {
                ++colons;
                ++j;
                if (j + 1 < raw.size() && std::isdigit(raw[j]) && std::isdigit(raw[j + 1])) {
                    j += 2;
                } else {
                    valid = false;
                    break;
                }
            } else if (std::isdigit(raw[j])) {
                ++j;
            } else {
                break;
            }
        }
        if (valid && colons >= 1 && colons <= 2 && j > start + 3) {
            return raw.substr(start, j - start);
        }
    }
    // Fallback
    std::string cleaned;
    for (char c : raw) {
        if (std::isdigit(c) || c == ':') cleaned += c;
    }
    return cleaned;
}

static std::string ocr_time(const TessAPI& api, TessBaseAPI* handle,
                            const cv::Mat& roi_bgr, bool is_active) {
    if (!handle || !api.set_image_raw || !api.get_text || !api.del_text) return "";

    cv::Mat scaled;
    cv::resize(roi_bgr, scaled, cv::Size(), 3.0, 3.0, cv::INTER_CUBIC);

    cv::Mat gray;
    cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);

    cv::Mat thresh;
    if (is_active) {
        cv::threshold(gray, thresh, 150, 255, cv::THRESH_BINARY);
    } else {
        cv::threshold(gray, thresh, 100, 255, cv::THRESH_BINARY_INV);
    }

    api.set_image_raw(handle, thresh.data, thresh.cols, thresh.rows, 1, static_cast<int>(thresh.step[0]));

    char* text = api.get_text(handle);
    std::string result;
    if (text) {
        result = extract_time_string(text);
        api.del_text(text);
    }
    return result;
}

ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo) {
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

    // Initialize Tesseract once via dynamic loading and ensure cleanup on exit
    struct TessContext {
        TessAPI api;
        TessBaseAPI* handle = nullptr;
        TessContext() {
            api = load_tesseract();
            if (api.create && api.init) {
                handle = api.create();
                const char* tessdata_path = "E:/vcpkg/installed/x64-windows/share/tessdata";
                if (api.init(handle, tessdata_path, "eng") == 0 && api.set_var) {
                    api.set_var(handle, "tessedit_char_whitelist", "0123456789: ");
                    api.set_var(handle, "tessedit_pageseg_mode", "7");
                } else {
                    if (api.del) api.del(handle);
                    handle = nullptr;
                }
            }
        }
        ~TessContext() { if (handle && api.del) api.del(handle); }
    };
    static TessContext ctx;

    state.white_time = ocr_time(ctx.api, ctx.handle, bot_bgr, state.active_player == "white");
    state.black_time = ocr_time(ctx.api, ctx.handle, top_bgr, state.active_player == "black");

    return state;
}

// ── Debug helpers ────────────────────────────────────────────────────────────

void generate_corner_debug_image(const cv::Mat& board_template, const std::string& output_dir) {
    std::filesystem::create_directories(output_dir);

    cv::Mat debug = board_template.clone();
    int bh = board_template.rows, bw = board_template.cols;
    double sq_h = bh / 8.0, sq_w = bw / 8.0;
    int ch = static_cast<int>(sq_h * 0.12);
    int cw = static_cast<int>(sq_w * 0.12);

    cv::Scalar color(255, 0, 255); // Bright Magenta
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = static_cast<int>(row * sq_h);
            int x1 = static_cast<int>(col * sq_w);
            int x2 = static_cast<int>((col + 1) * sq_w);
            int y2 = static_cast<int>((row + 1) * sq_h);

            cv::rectangle(debug, cv::Rect(x1, y1, cw, ch), color, -1);
            cv::rectangle(debug, cv::Rect(x2 - cw, y1, cw, ch), color, -1);
            cv::rectangle(debug, cv::Rect(x1, y2 - ch, cw, ch), color, -1);
            cv::rectangle(debug, cv::Rect(x2 - cw, y2 - ch, cw, ch), color, -1);
        }
    }

    cv::imwrite(output_dir + "/00_corner_debug_regions.png", debug);
}

} // namespace aa
