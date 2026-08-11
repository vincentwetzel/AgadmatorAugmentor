#pragma once

#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

namespace cta {
namespace DigitRecognizer {

struct SegmentDiagnostics {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    char symbol = '?';
};

struct RecognitionDiagnostics {
    std::string preprocessing_variant;
    std::string thresholding_mode;
    std::string selected_reading;
    std::vector<SegmentDiagnostics> segments;
    bool used_component_path = false;
};

// Recognizes chess clock time from a pre-cropped, thresholded, or scaled ROI.
// is_active indicates whether the text is expected to be dark-on-light (true) or light-on-dark (false).
std::string recognize_time(const cv::Mat& roi_bgr, bool is_active);

/// Runs the same OCR path as recognize_time while retaining calibration-only
/// provenance. The diagnostics are observational and do not affect selection.
std::string recognize_time_with_diagnostics(const cv::Mat& roi_bgr,
                                            bool is_active,
                                            RecognitionDiagnostics* diagnostics);

} // namespace DigitRecognizer
} // namespace cta
