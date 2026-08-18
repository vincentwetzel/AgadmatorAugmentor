#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include <cstdint>
#include "BoardLocalizer.h"

namespace cta {

inline constexpr double kMapperFineScanStepSeconds = 0.1;
inline constexpr double kMapperMotionBurstCapSeconds = 0.3;
inline constexpr double kMapperSettleConfirmationSeconds = 0.2;
inline constexpr double kMapperQuietCoarseScanDelaySeconds = 2.0;

struct CandidateFrame {
    // Stable within one extraction run: high 32 bits identify the source
    // chunk and low 32 bits identify emission order within that chunk.
    std::uint64_t observation_id = 0;
    // Decoder provenance for diagnosing whether a useful observation was
    // emitted by the mapper before detector/reducer processing began.
    std::uint32_t mapper_chunk = 0;
    std::uint64_t source_frame_index = 0;
    std::string emission_reason;
    // Populated only when the diagnostic frame artifact path is enabled.
    std::string diagnostic_frame_path;
    std::string diagnostic_board_path;
    std::string diagnostic_predecessor_board_path;
    std::string diagnostic_clock_top_path;
    std::string diagnostic_clock_bottom_path;
    double t;
    cv::Mat full_bgr;
    cv::Mat clock_top_bgr;
    cv::Mat clock_bot_bgr;
    cv::Mat board_bgr;
    cv::Mat board_gray;
    std::vector<double> board_hash;
    std::vector<std::string> yellow_arrows;
};

class VideoChunkMapper {
public:
    VideoChunkMapper(const std::string& safe_video_path, double duration, double chunk_duration, int total_chunks,
                     const BoardGeometry& geo, int margin_h, int margin_w, int debug_level,
                     bool has_clocks, int max_lookahead, int num_threads, int frame_width, int frame_height,
                     bool retain_full_frames = false,
                     double full_frame_interval_seconds = 5.0,
                     const std::string& diagnostic_frame_dir = std::string(),
                     double diagnostic_frame_start = -1.0,
                     double diagnostic_frame_end = -1.0,
                     const std::string& observation_replay_path = std::string());
    ~VideoChunkMapper();

    void start(std::atomic<bool>* cancel_flag);
    bool get_chunk_results(int chunk_idx, std::vector<CandidateFrame>& out_candidates, std::atomic<bool>* cancel_flag);
    bool peek_next_chunk_front(int chunk_idx, CandidateFrame& out_cf, std::atomic<bool>* cancel_flag);
    void consume_next_chunk_front(int chunk_idx);
    
    void set_current_reducing_chunk(int chunk_idx) { current_reducing_chunk_.store(chunk_idx); }
    long long get_candidates_emitted() const { return map_candidates_emitted_.load(); }
    bool has_failed() const { return map_failed_.load(); }
    std::string failure_reason() const;

private:
    void map_worker(int worker_idx, std::atomic<bool>* cancel_flag);
    void fail_mapping(const std::string& reason);

    std::string safe_video_path_;
    double duration_, chunk_duration_;
    int total_chunks_, margin_h_, margin_w_, debug_level_, max_lookahead_, num_threads_, frame_width_, frame_height_;
    bool retain_full_frames_ = false;
    double full_frame_interval_seconds_ = 5.0;
    std::string diagnostic_frame_dir_;
    double diagnostic_frame_start_ = -1.0;
    double diagnostic_frame_end_ = -1.0;
    std::string observation_replay_path_;
    int roi_x1_, roi_x2_, top_roi_y1_, top_roi_y2_, bot_roi_y1_, bot_roi_y2_;
    bool has_clocks_;
    BoardGeometry geo_;

    std::vector<std::vector<CandidateFrame>> chunk_results_;
    std::vector<bool> chunk_done_;
    std::atomic<int> current_reducing_chunk_{0}, next_chunk_to_map_{0};
    std::atomic<bool> map_failed_{false};
    std::atomic<long long> map_candidates_emitted_{0};
    mutable std::mutex results_mutex_;
    std::condition_variable results_cv_;
    std::vector<std::thread> workers_;
    std::string map_failure_reason_;
};
} // namespace cta
