#include "UIDetectors.h"
#include "BoardLocalizer.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cctype>
#include <vector>
#include <string>
#include <array>

namespace aa {

// ── Batch 64-square mean via integral image ──────────────────────────────────

std::vector<double> compute_all_square_means(const cv::Mat& img,
                                              const BoardGeometry& geo,
                                              int margin_h,
                                              int margin_w) {
    // Build integral image (sum type, 64-bit float for precision)
    cv::Mat integral_img;
    cv::integral(img, integral_img, CV_64F);

    std::vector<double> means(64);
    const int sq_w = static_cast<int>(geo.sq_w);
    const int sq_h = static_cast<int>(geo.sq_h);

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            // Apply margins to exclude edges
            int y1 = row * sq_h + margin_h;
            int y2 = (row + 1) * sq_h - margin_h;
            int x1 = col * sq_w + margin_w;
            int x2 = (col + 1) * sq_w - margin_w;

            // Clamp to image bounds
            y1 = std::max(0, std::min(y1, img.rows - 1));
            x1 = std::max(0, std::min(x1, img.cols - 1));
            y2 = std::max(y1 + 1, std::min(y2, img.rows));
            x2 = std::max(x1 + 1, std::min(x2, img.cols));

            // Integral image sum: sum = I(y2,x2) - I(y1,x2) - I(y2,x1) + I(y1,x1)
            double sum = integral_img.at<double>(y2, x2)
                       - integral_img.at<double>(y1, x2)
                       - integral_img.at<double>(y2, x1)
                       + integral_img.at<double>(y1, x1);

            int area = (y2 - y1) * (x2 - x1);
            // Map image row (0=top/rank8) to libchess index (0=a1/bottom)
            means[(7 - row) * 8 + col] = area > 0 ? sum / area : 0.0;
        }
    }
    return means;
}

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

// ── Clock extraction — Lightweight Digit Recognizer ──────────────────────────
// Replaces Tesseract with a fast Hu moments-based character classifier.
// Chess clocks use only digits 0-9 and ":" — a limited character set
// well-suited for shape-based classification.

struct DigitTemplate {
    std::array<double, 7> hu;  // Hu moments (log-transformed)
    double area;               // Normalized area ratio
    double aspect;             // Width/height ratio
};

// Pre-computed Hu moment templates for digits 0-9 and ":"
// These represent 7-segment digital clock style digit shapes
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

// Segment a thresholded clock image into individual character regions
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

// Classify a character segment against digit templates
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

// Recognize time from a clock ROI using lightweight digit classification
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

    // Validate: at least one colon, mostly digits
    int colons = 0, digits = 0;
    for (char c : result) {
        if (c == ':') ++colons;
        else if (std::isdigit(c)) ++digits;
    }
    if (colons < 1 || digits < 3) return "";

    return result;
}

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

    // ── Conditional OCR: quick diff check against cached ROIs ────────────
    cv::Mat top_gray, bot_gray;
    cv::cvtColor(top_bgr, top_gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(bot_bgr, bot_gray, cv::COLOR_BGR2GRAY);

    bool need_ocr = true;
    if (cache && cache->valid) {
        // Quick pixel diff against cached ROIs
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

        // If both clock ROIs haven't changed meaningfully (threshold: 5.0 mean pixel diff),
        // reuse cached times and skip OCR calls
        if (top_diff < 5.0 && bot_diff < 5.0) {
            state.white_time = cache->white_time;
            state.black_time = cache->black_time;
            state.ocr_skipped = true;
            need_ocr = false;
        }
    }

    if (need_ocr) {
        // Lightweight Hu moments-based digit recognizer
        // No external dependencies — runs in microseconds vs Tesseract's milliseconds
        state.white_time = recognize_time(bot_bgr, state.active_player == "white");
        state.black_time = recognize_time(top_bgr, state.active_player == "black");
        state.ocr_skipped = false;

        // Update cache if provided
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
