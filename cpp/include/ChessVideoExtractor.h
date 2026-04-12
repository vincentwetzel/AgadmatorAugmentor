#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <opencv2/core/mat.hpp>

// Forward declarations to avoid including heavy headers
namespace libchess { class Position; }
namespace aa { class FramePrefetcher; class GPUPipeline; struct BoardGeometry; struct ClockCache; }

namespace aa {

struct GameData {
    std::vector<std::string> moves;
    std::vector<double> timestamps;
    std::vector<std::string> fens;
    struct ClockInfo {
        std::string active;
        std::string white_time;
        std::string black_time;
    };
    std::vector<ClockInfo> clocks;
};

enum class DebugLevel {
    None,
    Moves,
    Full
};

class ChessVideoExtractor {
public:
    ChessVideoExtractor(const std::string& board_asset_path,
                        const std::string& red_board_asset_path = "",
                        DebugLevel debug_level = DebugLevel::None);
    ~ChessVideoExtractor();

    using ProgressCallback = std::function<void(int percent, const std::string& message)>;
    void set_progress_callback(ProgressCallback cb);

    GameData extract_moves_from_video(const std::string& video_path,
                                      const std::string& output_path,
                                      const std::string& debug_label = "");

private:
    struct MoveScore;
    struct ScratchBuffers;

    cv::Mat get_max_square_diff(const cv::Mat& img_a, const cv::Mat& img_b);
    MoveScore score_moves_for_board(const std::vector<double>& sq_diffs);

    DebugLevel debug_level_;
    cv::Mat board_template_;
    cv::Mat red_board_template_;
    std::unique_ptr<BoardGeometry> geo_;
    int margin_h_ = 0, margin_w_ = 0;
    std::unique_ptr<GPUPipeline> gpu_pipeline_;
    bool gpu_pipeline_active_ = false;
    std::unique_ptr<ClockCache> clock_cache_;
    std::unique_ptr<libchess::Position> pos_ptr_;
    std::unique_ptr<FramePrefetcher> prefetcher_;
    std::unique_ptr<ScratchBuffers> scratch_;
    ProgressCallback progress_callback_;
};

} // namespace aa