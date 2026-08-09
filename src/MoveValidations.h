#pragma once
#include <opencv2/opencv.hpp>
#include <array>
#include <cstddef>
#include "UIDetectors.h" // Ensures BoardGeometry is defined

namespace cta {
namespace validation {

inline constexpr double kYellowEndpointThreshold = 25.0;
inline constexpr double kYellowPairThreshold = 70.0;

struct YellowSquareMeasurement {
    std::array<double, 4> corner_scores{};
    std::array<std::array<double, 3>, 4> corner_bgr{};
    std::array<double, 4> corner_edge_density{};
    double score = 0.0;
};

struct HoverBoxMeasurement {
    double top_edge = 0.0;
    double bottom_edge = 0.0;
    double left_edge = 0.0;
    double right_edge = 0.0;
    double strongest_edge = 0.0;
    std::size_t visible_edges = 0;
    bool detected = false;
};

double check_yellowness(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* sq_name);

YellowSquareMeasurement measure_yellowness(const cv::Mat& board_bgr,
                                           const BoardGeometry& geo,
                                           const char* sq_name,
                                           bool include_edge_metrics = false);

HoverBoxMeasurement measure_hover_box(const cv::Mat& board_bgr,
                                      const BoardGeometry& geo,
                                      cv::Mat& white_mask,
                                      cv::Mat& reduced,
                                      const char* sq_name);

bool check_hover_box(const cv::Mat& board_bgr, const BoardGeometry& geo, cv::Mat& white_mask, cv::Mat& reduced, const char* sq_name);

} // namespace validation
} // namespace cta
