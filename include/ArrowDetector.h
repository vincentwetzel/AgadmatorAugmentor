#pragma once

#include <opencv2/core/mat.hpp>
#include <string>
#include <vector>

namespace cta {

struct BoardGeometry;

// ── Yellow arrow detection ───────────────────────────────────────────────────

/// Finds yellow arrows drawn on the board by the streamer to indicate tactics.
/// Uses HSV masking, morphological cleanup, ray-casting between active squares,
/// overlap suppression, and endpoint direction detection.
/// @return Sorted list of arrow strings (e.g. {"e2e4", "f1b5"}).
std::vector<std::string> find_yellow_arrows(const cv::Mat& img_bgr,
                                            const cv::Mat& board_template,
                                            const BoardGeometry& geo);

} // namespace cta
