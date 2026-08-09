// Extracted from cpp directory
#include "BoardAnalysis.h"
#include "BoardLocalizer.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <string>

namespace cta {

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
            double min_visible = 1.0;
            for (double edge_ratio : ratios) {
                if (edge_ratio > 0.10) {
                    ++visible_edges;
                    min_visible = std::min(min_visible, edge_ratio);
                }
                sum += edge_ratio;
            }

            // A dragged piece hover box is a near-rectangular white outline. Requiring
            // three strong projected edges prevents scattered bright UI/piece pixels
            // from winning on unrelated squares while still tolerating one occluded side.
            if (visible_edges >= 2 && min_visible > 0.18 && sum > 1.35 && sum > max_score) {
                max_score = sum;
                char sq[3];
                sq[0] = 'a' + c;
                sq[1] = '0' + (8 - r);
                sq[2] = '\0';
                best_square = sq;
            }
        }
    }
    if (best_square.empty()) {
        // Some board themes render the hover outline as a clipped white
        // cursor/handle instead of four complete edges.  In that case use
        // isolated connected components as a generic secondary signal. The
        // size and shape limits exclude most piece silhouettes and board
        // coordinate labels without depending on a particular frame.
        cv::Mat component_labels, component_stats, component_centroids;
        const int component_count = cv::connectedComponentsWithStats(
            white_mask, component_labels, component_stats, component_centroids, 8, CV_32S);
        double best_component_score = 0.0;
        for (int component = 1; component < component_count; ++component) {
            const int x = component_stats.at<int>(component, cv::CC_STAT_LEFT);
            const int y = component_stats.at<int>(component, cv::CC_STAT_TOP);
            const int width = component_stats.at<int>(component, cv::CC_STAT_WIDTH);
            const int height = component_stats.at<int>(component, cv::CC_STAT_HEIGHT);
            const int area = component_stats.at<int>(component, cv::CC_STAT_AREA);
            if (area < 50 || area > 180 || width < 6 || height < 6 || width > 22 || height > 22) {
                continue;
            }
            if (x <= 1 || y <= 1 || x + width >= white_mask.cols - 1 ||
                y + height >= white_mask.rows - 1) {
                continue;
            }

            const double aspect = static_cast<double>(std::min(width, height)) /
                static_cast<double>(std::max(width, height));
            const double fill = static_cast<double>(area) /
                static_cast<double>(width * height);
            if (aspect < 0.55 || fill < 0.35) {
                continue;
            }

            const double center_x = component_centroids.at<double>(component, 0);
            const double center_y = component_centroids.at<double>(component, 1);
            const int col = std::clamp(static_cast<int>(center_x / geo.sq_w), 0, 7);
            const int row = std::clamp(static_cast<int>(center_y / geo.sq_h), 0, 7);
            const double component_score = static_cast<double>(area) * aspect *
                (0.5 + 0.5 * fill);
            if (component_score <= best_component_score) {
                continue;
            }

            best_component_score = component_score;
            char sq[3];
            sq[0] = 'a' + col;
            sq[1] = '0' + (8 - row);
            sq[2] = '\0';
            best_square = sq;
        }
    }
    if (best_square.empty() && !board_template.empty()) {
        cv::Mat resized_template;
        cv::resize(board_template, resized_template, board_img.size(), 0, 0, cv::INTER_AREA);
        cv::Mat board_gray;
        cv::Mat tpl_gray;
        cv::cvtColor(board_img, board_gray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(resized_template, tpl_gray, cv::COLOR_BGR2GRAY);

        cv::Mat diff;
        cv::absdiff(board_gray, tpl_gray, diff);
        double best_diff = 0.0;
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                int y1 = static_cast<int>(r * geo.sq_h + geo.sq_h * 0.08);
                int y2 = static_cast<int>((r + 1) * geo.sq_h - geo.sq_h * 0.08);
                int x1 = static_cast<int>(c * geo.sq_w + geo.sq_w * 0.08);
                int x2 = static_cast<int>((c + 1) * geo.sq_w - geo.sq_w * 0.08);
                if (x2 <= x1 || y2 <= y1) {
                    continue;
                }

                double mean_diff = cv::mean(diff(cv::Rect(x1, y1, x2 - x1, y2 - y1)))[0];
                if (mean_diff > best_diff) {
                    best_diff = mean_diff;
                    char sq[3];
                    sq[0] = 'a' + c;
                    sq[1] = '0' + (8 - r);
                    sq[2] = '\0';
                    best_square = sq;
                }
            }
        }
        if (best_diff < 18.0) {
            best_square.clear();
        }
    }
    return best_square;
}

} // namespace cta
