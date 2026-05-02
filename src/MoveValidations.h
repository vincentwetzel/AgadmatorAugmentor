#pragma once
#include <opencv2/opencv.hpp>
#include "UIDetectors.h" // Ensures BoardGeometry is defined

namespace cta {
namespace validation {

double check_yellowness(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* sq_name);

bool check_hover_box(const cv::Mat& board_bgr, const BoardGeometry& geo, cv::Mat& white_mask, cv::Mat& reduced, const char* sq_name);

} // namespace validation
} // namespace cta