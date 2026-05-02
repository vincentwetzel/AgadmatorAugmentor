// Extracted from cpp directory
#include "BoardAnalysis.h"
#include "BoardLocalizer.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cctype>
#include <vector>
#include <string>
#include <array>

namespace cta {

// ── Batch 64-square mean via integral image ──────────────────────────────────

std::vector<double> compute_all_square_means(const cv::Mat& img,
                                              const BoardGeometry& geo,
                                              int margin_h,
                                              int margin_w) {
    cv::Mat integral_img;
    cv::integral(img, integral_img, CV_64F);

    std::vector<double> means(64);
    const int sq_w = static_cast<int>(geo.sq_w);
    const int sq_h = static_cast<int>(geo.sq_h);

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = row * sq_h + margin_h;
            int y2 = (row + 1) * sq_h - margin_h;
            int x1 = col * sq_w + margin_w;
            int x2 = (col + 1) * sq_w - margin_w;

            y1 = std::max(0, std::min(y1, img.rows - 1));
            x1 = std::max(0, std::min(x1, img.cols - 1));
            y2 = std::max(y1 + 1, std::min(y2, img.rows));
            x2 = std::max(x1 + 1, std::min(x2, img.cols));

            double sum = integral_img.at<double>(y2, x2)
                       - integral_img.at<double>(y1, x2)
                       - integral_img.at<double>(y2, x1)
                       + integral_img.at<double>(y1, x1);

            int area = (y2 - y1) * (x2 - x1);
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

    cv::Mat board_float;
    board_img.convertTo(board_float, CV_32FC3);
    std::vector<cv::Mat> channels;
    cv::split(board_float, channels);
    cv::Mat yellowness_map = (channels[2] + channels[1]) / 2.0f - channels[0];

    std::vector<std::pair<double, int>> sq_scores;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            double score = mean_corner_yellowness(board_img, row, col, geo.sq_h, geo.sq_w);
            sq_scores.emplace_back(score, row * 8 + col);
        }
    }

    std::sort(sq_scores.begin(), sq_scores.end(), std::greater<>());
    int idx1 = sq_scores[0].second;
    int idx2 = sq_scores[1].second;

    int row1 = idx1 / 8, col1 = idx1 % 8;
    int row2 = idx2 / 8, col2 = idx2 % 8;

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

// ── Hover box detection ──────────────────────────────────────────────────────

std::string find_misaligned_piece(const cv::Mat& img_bgr,
                                  const cv::Mat& board_template,
                                  const BoardGeometry& geo) {
    cv::Mat board_img = img_bgr(cv::Rect(geo.bx, geo.by, geo.bw, geo.bh));

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

// ── Debug helpers ────────────────────────────────────────────────────────────

void generate_corner_debug_image(const cv::Mat& board_template, const std::string& output_dir) {
    std::filesystem::create_directories(output_dir);

    cv::Mat debug = board_template.clone();
    int bh = board_template.rows, bw = board_template.cols;
    double sq_h = bh / 8.0, sq_w = bw / 8.0;
    int ch = static_cast<int>(sq_h * 0.12);
    int cw = static_cast<int>(sq_w * 0.12);

    cv::Scalar color(255, 0, 255);
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

} // namespace cta
