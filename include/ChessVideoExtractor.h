#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <map>
#include <atomic>
#include <opencv2/core/mat.hpp>

// Forward declarations to avoid including heavy headers
namespace libchess { class Position; }
namespace cta { class GPUPipeline; struct BoardGeometry; struct ClockCache; }

namespace cta {

struct ClockInfo {
    std::string active;
    std::string white_time;
    std::string black_time;
    // True only when the clock belonging to the move that produced this
    // state was read directly and passed temporal sanity checks. Revert and
    // replay handling may retain a settled parent clock for continuity, but
    // must not turn that inherited value into a new clock observation.
    bool moved_time_observed = false;
    // The moved clock could not be parsed at all. This is distinct from a
    // parsed reading that failed a temporal sanity check.
    bool moved_time_missing = false;
    // Provenance of the moved-side reading: initial, direct, contextual,
    // temporal, inherited, missing, or rejected.
    std::string moved_time_provenance = "missing";
};

// A struct to hold a single analysis branch (a sequence of moves not on the main line)
struct VariationData {
    std::vector<std::string> moves;
    std::vector<double> timestamps;
    std::vector<std::string> fens;
    // True when this line was observed while the board was being replayed
    // rather than played as an independently clocked game continuation.
    bool replay_observation = false;
    // Visual move confidence captured when the branch was observed.  This is
    // kept with the variation so later replay/revert normalization can make
    // decisions from detector evidence instead of UCI notation.
    std::vector<double> scores;
    // Use the same ClockInfo struct as GameData for consistency
    std::vector<ClockInfo> clocks;
};

struct GameData {
    std::vector<std::string> moves;
    std::vector<double> timestamps;
    std::vector<std::string> fens;
    std::vector<ClockInfo> clocks;
    // A map from a ply's 0-based index to a list of variations that branch from it
    std::map<size_t, std::vector<VariationData>> variations;

    std::vector<std::string> video_fens;
    // Timestamps used by analysis-video overlays. These intentionally follow
    // the source board's visual update time, which may be earlier than the
    // settled verification timestamp in timestamps.
    std::vector<double> video_timestamps;
    std::vector<std::string> video_moves;
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
                        DebugLevel debug_level = DebugLevel::None,
                        int memory_limit_mb = 0);
    ~ChessVideoExtractor();

    using ProgressCallback = std::function<void(int percent, const std::string& message)>;
    void set_progress_callback(ProgressCallback cb);

    using FenDetectedCallback = std::function<void(const std::string&)>;
    void set_fen_detected_callback(FenDetectedCallback cb) { fen_cb_ = std::move(cb); }

    GameData extract_moves_from_video(const std::string& video_path,
                                      const std::string& debug_label = "",
                                      std::atomic<bool>* cancel_flag = nullptr);

    const BoardGeometry* get_board_geometry() const;

private:
    struct MoveScore;
    struct ScratchBuffers;

    cv::Mat get_max_square_diff(const cv::Mat& img_a, const cv::Mat& img_b);
    MoveScore score_moves_for_board(const std::vector<double>& sq_diffs);

    DebugLevel debug_level_;
    int memory_limit_mb_ = 0;
    cv::Mat board_template_;
    cv::Mat red_board_template_;
    std::unique_ptr<BoardGeometry> geo_;
    int margin_h_ = 0, margin_w_ = 0;
    std::unique_ptr<GPUPipeline> gpu_pipeline_;
    bool gpu_pipeline_active_ = false;
    std::unique_ptr<ClockCache> clock_cache_;
    std::unique_ptr<libchess::Position> pos_ptr_;
    std::unique_ptr<ScratchBuffers> scratch_;
    ProgressCallback progress_callback_;
    FenDetectedCallback fen_cb_;
};

} // namespace cta
