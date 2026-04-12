#include "ArrowDetector.h"
#include "BoardLocalizer.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace aa {

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

    // Pre-filter active squares (>1.5% coverage)
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

    // Sort by length, longest first
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
            // Thick suppression shadow (1.8x square width)
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

} // namespace aa
