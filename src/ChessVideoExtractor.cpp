// Extracted from cpp directory
#include "ChessVideoExtractor.h"
#include "ChessVideoExtractor_Internal.h"
#include "UIDetectors.h"
#include "BoardAnalysis.h"
#include "GPUAccelerator.h"
#include "BoardLocalizer.h"
#include "ExtractorUtils.h"
#include "MoveValidations.h"
#include "MoveScorer.h"
#include "RevertManager.h"
#include "VideoChunkMapper.h"
#include "BoardCache.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include "libchess/square.hpp"
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <array>
#include <optional>
#include <limits>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <unordered_map>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace cta {

// ── Constructor ──────────────────────────────────────────────────────────────

ChessVideoExtractor::ChessVideoExtractor(const std::string& board_asset_path,
                                          const std::string& red_board_asset_path,
                                          DebugLevel debug_level,
                                          int memory_limit_mb)
    : debug_level_(debug_level), memory_limit_mb_(memory_limit_mb) {
    std::string safe_board_path = utils::get_safe_path(board_asset_path);
    board_template_ = cv::imread(safe_board_path);
    if (board_template_.empty()) {
        throw std::runtime_error("Could not load board asset at: " + board_asset_path);
    }

    if (!red_board_asset_path.empty()) {
        std::string safe_red_path = utils::get_safe_path(red_board_asset_path);
        red_board_template_ = cv::imread(safe_red_path);
    }
}

ChessVideoExtractor::~ChessVideoExtractor() = default;

void ChessVideoExtractor::set_progress_callback(ProgressCallback cb) {
    progress_callback_ = std::move(cb);
}

const BoardGeometry* ChessVideoExtractor::get_board_geometry() const {
    return geo_.get();
}

// ── Square diff calculation ──────────────────────────────────────────────────

cv::Mat ChessVideoExtractor::get_max_square_diff(const cv::Mat& img_a, const cv::Mat& img_b) {
    cv::Mat diff;
    GPUAccelerator::absdiff(img_a, img_b, diff);

    double max_val = 0;
    cv::minMaxLoc(diff, nullptr, &max_val);
    if (max_val < 15.0) return cv::Mat();

    // Batch compute all 64 square means via integral image
    auto sq_means = compute_all_square_means(diff, *geo_, margin_h_, margin_w_);
    double max_sq_diff = 0.0;
    for (double sd : sq_means) {
        if (sd > max_sq_diff) max_sq_diff = sd;
    }

    if (max_sq_diff <= 15.0) return cv::Mat();
    return diff;
}

// ── Move scoring using libchess ──────────────────────────────────────────────

ChessVideoExtractor::MoveScore ChessVideoExtractor::score_moves_for_board(const std::vector<double>& sq_diffs) {
    if (!pos_ptr_) return {};
    auto best = MoveScorer::score_moves_for_board(*pos_ptr_, sq_diffs);
    return {best.from_sq, best.to_sq, best.promotion, best.score};
}



} // namespace cta
