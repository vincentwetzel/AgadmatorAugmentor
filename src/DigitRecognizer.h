#pragma once

#include <string>
#include <opencv2/opencv.hpp>

namespace cta {
namespace DigitRecognizer {

// Recognizes chess clock time from a pre-cropped, thresholded, or scaled ROI.
// is_active indicates whether the text is expected to be dark-on-light (true) or light-on-dark (false).
std::string recognize_time(const cv::Mat& roi_bgr, bool is_active);

} // namespace DigitRecognizer
} // namespace cta