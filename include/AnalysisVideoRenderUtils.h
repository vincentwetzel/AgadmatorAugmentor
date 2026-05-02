#pragma once

#include <string>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <vector>
#include <array>
#include <optional>
#include <algorithm> // For std::clamp
#include <cmath>     // For std::abs

#include "StockfishAnalyzer.h" // For StockfishLine

namespace cta {
namespace AnalysisVideoRenderUtils {

// Forward declaration needed because ChessFenUtils can't include this header.
// So ChessFenUtils functions that use EngineArrowStyle have to be called
// with the fully qualified cta::AnalysisVideoRenderUtils::EngineArrowStyle.
struct EngineArrowStyle {
    cv::Scalar tail_color;
    cv::Scalar head_color;
    double opacity = 0.0;
    int thickness_pct = 0;
};

cv::Scalar arrow_gradient_color_at(cv::Point start, cv::Point end, int x, int y, const EngineArrowStyle& style);
void drawEngineArrow(cv::Mat& overlay, cv::Point start, cv::Point end, cv::Scalar color, double squareSize, int thicknessPct);
void blend_arrow_on_bgr(cv::Mat& image, cv::Point start, cv::Point end, const EngineArrowStyle& style, double squareSize);
void blend_arrow_on_bgra(cv::Mat& image, cv::Point start, cv::Point end, const EngineArrowStyle& style, double squareSize);
void drawMoveAnnotationOnBoard(cv::Mat& img, const std::string& uci, const std::string& sym, double sq_w, double sq_h);
void drawAnalysisBar(cv::Mat& img, cv::Rect rect, double cpScore);
EngineArrowStyle compute_engine_arrow_style(int line_index, double diff_cp, int arrow_thickness_pct);
void render_main_board_arrows(cv::Mat& image,
                              const std::optional<StockfishResult>& analysis,
                              const std::string& fen,
                              int width, int height,
                              int arrow_thickness_pct);

} // namespace AnalysisVideoRenderUtils
} // namespace cta
