#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include "BoardLocalizer.h"

namespace cta {

struct CandidateFrame {
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
                     bool has_clocks, int max_lookahead, int num_threads, int frame_width, int frame_height);
    ~VideoChunkMapper();

    void start(std::atomic<bool>* cancel_flag);
    bool get_chunk_results(int chunk_idx, std::vector<CandidateFrame>& out_candidates, std::atomic<bool>* cancel_flag);
    bool peek_next_chunk_front(int chunk_idx, CandidateFrame& out_cf, std::atomic<bool>* cancel_flag);
    void consume_next_chunk_front(int chunk_idx);
    
    void set_current_reducing_chunk(int chunk_idx) { current_reducing_chunk_.store(chunk_idx); }
    long long get_candidates_emitted() const { return map_candidates_emitted_.load(); }
    bool has_failed() const { return map_failed_.load(); }

private:
    void map_worker(int worker_idx, std::atomic<bool>* cancel_flag);

    std::string safe_video_path_;
    double duration_, chunk_duration_;
    int total_chunks_, margin_h_, margin_w_, debug_level_, max_lookahead_, num_threads_, frame_width_, frame_height_;
    int roi_x1_, roi_x2_, top_roi_y1_, top_roi_y2_, bot_roi_y1_, bot_roi_y2_;
    bool has_clocks_;
    BoardGeometry geo_;

    std::vector<std::vector<CandidateFrame>> chunk_results_;
    std::vector<bool> chunk_done_;
    std::atomic<int> current_reducing_chunk_{0}, next_chunk_to_map_{0};
    std::atomic<bool> map_failed_{false};
    std::atomic<long long> map_candidates_emitted_{0};
    std::mutex results_mutex_; std::condition_variable results_cv_; std::vector<std::thread> workers_;
};
} // namespace cta
