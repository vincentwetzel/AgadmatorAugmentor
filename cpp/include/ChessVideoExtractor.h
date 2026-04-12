#pragma once

#include "BoardLocalizer.h"
#include "UIDetectors.h"  // for ClockCache
#include "GPUAccelerator.h"  // for GPUPipeline
#include <opencv2/opencv.hpp>
#include "libchess/move.hpp"
#include <string>
#include <vector>
#include <memory>

// Forward declare libchess types — headers included in .cpp
namespace libchess { class Position; }

// Forward declare frame prefetcher
namespace aa { class FramePrefetcher; }

// GPU accelerator is included via GPUAccelerator.h (pulled in by .cpp)

namespace aa {

/// Debug image output level.
enum class DebugLevel {
    None,   // No debug images
    Moves,  // Save one debug image per detected move
    Full    // Save before/after/diff images for every move
};

/// Extracted game data (mirrors the Python JSON output).
struct GameData {
    std::vector<std::string> moves;         // UCI notation
    std::vector<double> timestamps;          // Seconds into video
    std::vector<std::string> fens;           // FEN after each ply
    struct ClockEntry {
        std::string active;
        std::string white_time;
        std::string black_time;
    };
    std::vector<ClockEntry> clocks;
};

/// Orchestrator: scans a video, detects moves via UI elements, validates with chess logic.
///
/// Uses an adaptive FAST/FINE scanning strategy:
///   - FAST mode: polls every 1.0s, checks for board changes via square diffs.
///   - FINE mode: once a change is detected, backs up 0.8s and scans at 0.2s intervals
///     to precisely locate the moment the move settled.
class ChessVideoExtractor {
public:
    /// @param board_asset_path   Path to the pristine board image (board.png).
    /// @param red_board_asset_path Optional path to a board image with red highlights.
    /// @param debug_level        Level of debug image output.
    explicit ChessVideoExtractor(const std::string& board_asset_path,
                                 const std::string& red_board_asset_path = "",
                                 DebugLevel debug_level = DebugLevel::Moves);

    /// Extracts moves from a video file.
    /// @param video_path  Path to the input video.
    /// @param output_path Path to write the output JSON.
    /// @param debug_label Label used for debug directory naming.
    GameData extract_moves_from_video(const std::string& video_path,
                                      const std::string& output_path,
                                      const std::string& debug_label = "");

    ~ChessVideoExtractor();

private:
    cv::Mat board_template_;
    cv::Mat red_board_template_;
    DebugLevel debug_level_;

    // libchess position state
    std::unique_ptr<libchess::Position> pos_ptr_;

    // Margins for batch square mean computation (integral image)
    BoardGeometry geo_;
    int margin_h_ = 0, margin_w_ = 0;

    // Scratch buffers to avoid per-frame allocations in hot paths
    struct ScratchBuffers {
        std::vector<cv::Mat> channels;   // reused by convertTo+split
        cv::Mat float_mat;               // CV_32FC3 for yellowness
        cv::Mat white_mask;              // hover box detection
        cv::Mat reduced;                 // for cv::reduce output
    } scratch_;

    // Clock OCR cache — avoids redundant Tesseract calls when pixels unchanged
    ClockCache clock_cache_;

    // Frame prefetcher — async video I/O hiding
    std::unique_ptr<FramePrefetcher> prefetcher_;

    // Zero-copy GPU pipeline — keeps grayscale frames on GPU to eliminate
    /// per-frame Host↔Device memory copies during absdiff + integral.
    GPUPipeline gpu_pipeline_;
    bool gpu_pipeline_active_ = false;

    cv::Mat get_max_square_diff(const cv::Mat& img_a, const cv::Mat& img_b);

    /// GPU-accelerated square diff means using the zero-copy pipeline.
    /// Returns empty vector if pipeline unavailable (caller falls back to CPU).
    std::vector<double> get_square_diff_means_gpu();

    struct MoveScore {
        int from_sq = -1, to_sq = -1;
        double score = -1.0;
        libchess::Move move;
    };
    MoveScore score_moves_for_board(const cv::Mat& diff_image);
};

} // namespace aa
