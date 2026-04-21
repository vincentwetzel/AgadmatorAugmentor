#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>
#include <atomic>
#include <opencv2/core.hpp>
#include "StockfishAnalyzer.h"
#include "BoardLocalizer.h"
#include "VideoOverlayConfig.h"

namespace aa {

class AnalysisVideoGenerator {
public:
    // Initialize with the path to the 'assets' directory
    explicit AnalysisVideoGenerator(const std::string& assets_dir);

    /**
     * Generates a composited video with analysis overlays.
     * @param input_video_path Source video to read from
     * @param output_video_path Destination path for the new MP4
     * @param geo The geometry of the main board in the video
     * @param fens List of FEN strings (starting pos + subsequent moves)
     * @param timestamps List of timestamps (in seconds) corresponding to each move
     * @param stockfish_results Analysis data from Stockfish
     * @param arrow_thickness_pct Thickness of engine arrows as a percentage
     * @param overlay_config The visual overlay configuration (positions, scales, toggles)
     * @param cancel_flag Atomic flag to signal cancellation
     * @param progress_callback Optional callback to report completion percentage and status text
     */
    bool generate_analysis_video(const std::string& input_video_path, 
                                 const std::string& output_video_path, 
                                 const BoardGeometry& geo,
                                 const std::vector<std::string>& fens,
                                 const std::vector<double>& timestamps,
                                 const std::vector<StockfishResult>& stockfish_results,
                                 int arrow_thickness_pct,
                                 const VideoOverlayConfig& overlay_config,
                                 std::atomic<bool>* cancel_flag,
                                 std::function<void(int, const std::string&)> progress_callback = nullptr);

private:
    cv::Mat board_template_;
    std::map<char, cv::Mat> piece_assets_;

    cv::Mat render_board_state(const std::string& fen, 
                               const std::optional<StockfishResult>& analysis, 
                               int arrow_thickness_pct,
                               const cv::Mat& scaled_board,
                               const std::map<char, cv::Mat>& scaled_pieces);
    void render_analysis_text(cv::Mat& image,
                              const std::optional<StockfishResult>& analysis,
                              const std::string& fen,
                              int width,
                              int height) const;
    void render_analysis_bar(cv::Mat& image,
                             const std::optional<StockfishResult>& analysis,
                             const std::string& fen,
                             int width,
                             int height) const;
    void load_piece_assets(const std::string& assets_dir);
    void overlay_image(cv::Mat& background, const cv::Mat& foreground, cv::Point location);
};

} // namespace aa
