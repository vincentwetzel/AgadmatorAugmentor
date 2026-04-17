#include "MoveValidations.h"
#include "BoardLocalizer.h"
#include <algorithm>

namespace aa {
namespace validation {

double check_yellowness(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* sq_name) {
    int col = sq_name[0] - 'a';
    int rank = sq_name[1] - '1';
    int row = 7 - rank;
    int y1 = static_cast<int>(row * geo.sq_h);
    int y2 = static_cast<int>((row + 1) * geo.sq_h);
    int x1 = static_cast<int>(col * geo.sq_w);
    int x2 = static_cast<int>((col + 1) * geo.sq_w);
    int ch = static_cast<int>(geo.sq_h * 0.12);
    int cw = static_cast<int>(geo.sq_w * 0.12);

    // Clamp to frame bounds
    int fh = board_bgr.rows, fw = board_bgr.cols;
    x1 = std::max(0, std::min(x1, fw - 1));
    y1 = std::max(0, std::min(y1, fh - 1));
    x2 = std::max(x1 + 1, std::min(x2, fw));
    y2 = std::max(y1 + 1, std::min(y2, fh));

    cv::Rect corners[4] = {
        {x1, y1, std::min(cw, x2 - x1), std::min(ch, y2 - y1)},
        {std::max(x1, x2 - cw), y1, std::min(cw, x2 - std::max(x1, x2 - cw)), std::min(ch, y2 - y1)},
        {x1, std::max(y1, y2 - ch), std::min(cw, x2 - x1), std::min(ch, y2 - std::max(y1, y2 - ch))},
        {std::max(x1, x2 - cw), std::max(y1, y2 - ch), std::min(cw, x2 - std::max(x1, x2 - cw)), std::min(ch, y2 - std::max(y1, y2 - ch))}
    };

    double y_score = 0;
    for (const auto& c : corners) {
        if (c.width <= 0 || c.height <= 0) continue;
        cv::Mat patch = board_bgr(c);
        double sum_y = 0.0;
        for (int r = 0; r < patch.rows; ++r) {
            const auto* ptr = patch.ptr<cv::Vec3b>(r);
            for (int pc = 0; pc < patch.cols; ++pc) {
                sum_y += (ptr[pc][2] + ptr[pc][1]) / 2.0 - ptr[pc][0];
            }
        }
        y_score += sum_y / (patch.rows * patch.cols);
    }
    return y_score / 4.0;
}

bool check_hover_box(const cv::Mat& board_bgr, const BoardGeometry& geo, cv::Mat& white_mask, cv::Mat& reduced, const char* sq_name) {
    int col = sq_name[0] - 'a';
    int rank = sq_name[1] - '1';
    int row = 7 - rank;
    int y1 = static_cast<int>(row * geo.sq_h);
    int y2 = static_cast<int>((row + 1) * geo.sq_h);
    int x1 = static_cast<int>(col * geo.sq_w);
    int x2 = static_cast<int>((col + 1) * geo.sq_w);

    // Clamp to frame bounds
    int fh = board_bgr.rows, fw = board_bgr.cols;
    x1 = std::max(0, std::min(x1, fw - 1));
    y1 = std::max(0, std::min(y1, fh - 1));
    x2 = std::max(x1 + 1, std::min(x2, fw));
    y2 = std::max(y1 + 1, std::min(y2, fh));

    int sw = x2 - x1, sh = y2 - y1;
    if (white_mask.rows < sh || white_mask.cols < sw) {
        white_mask = cv::Mat(sh, sw, CV_8UC1);
    }
    cv::Mat white_mask_roi = white_mask(cv::Rect(0, 0, sw, sh));
    cv::inRange(board_bgr(cv::Rect(x1, y1, sw, sh)), cv::Scalar(160, 160, 160), cv::Scalar(255, 255, 255), white_mask_roi);

    int thickness = std::max(3, static_cast<int>(geo.sq_w * 0.08));
    cv::Mat top = white_mask_roi(cv::Rect(0, 0, sw, thickness));
    cv::Mat bottom = white_mask_roi(cv::Rect(0, sh - thickness, sw, thickness));
    cv::Mat left = white_mask_roi(cv::Rect(0, 0, thickness, sh));
    cv::Mat right = white_mask_roi(cv::Rect(sw - thickness, 0, thickness, sh));

    cv::reduce(top, reduced, 0, cv::REDUCE_MAX);
    double r0 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sw);
    cv::reduce(bottom, reduced, 0, cv::REDUCE_MAX);
    double r1 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sw);
    cv::reduce(left, reduced, 1, cv::REDUCE_MAX);
    double r2 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sh);
    cv::reduce(right, reduced, 1, cv::REDUCE_MAX);
    double r3 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sh);

    int visible = (r0 > 0.10) + (r1 > 0.10) + (r2 > 0.10) + (r3 > 0.10);
    return visible >= 2;
}

} // namespace validation
} // namespace aa