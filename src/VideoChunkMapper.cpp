#include "VideoChunkMapper.h"
#include "BoardAnalysis.h"
#include <cmath>
#include <algorithm>
#include <chrono>

namespace cta {

VideoChunkMapper::VideoChunkMapper(const std::string& safe_video_path, double duration, double chunk_duration, int total_chunks,
                                   const BoardGeometry& geo, int margin_h, int margin_w, int debug_level,
                                   bool has_clocks, int max_lookahead, int num_threads, int frame_width, int frame_height)
    : safe_video_path_(safe_video_path), duration_(duration), chunk_duration_(chunk_duration), total_chunks_(total_chunks),
      geo_(geo), margin_h_(margin_h), margin_w_(margin_w), debug_level_(debug_level), has_clocks_(has_clocks),
      max_lookahead_(max_lookahead), num_threads_(num_threads), frame_width_(frame_width), frame_height_(frame_height),
      chunk_results_(total_chunks), chunk_done_(total_chunks, false) {
    roi_x1_ = std::max(0, static_cast<int>(geo_.bx + geo_.bw * 0.70));
    roi_x2_ = std::min(frame_width_, static_cast<int>(geo_.bx + geo_.bw));
    top_roi_y1_ = std::max(0, static_cast<int>(geo_.by - geo_.sq_h * 0.55));
    top_roi_y2_ = std::max(top_roi_y1_ + 1, static_cast<int>(geo_.by - geo_.sq_h * 0.08));
    bot_roi_y1_ = std::min(frame_height_ - 1, static_cast<int>(geo_.by + geo_.bh + geo_.sq_h * 0.18));
    bot_roi_y2_ = std::min(frame_height_, static_cast<int>(geo_.by + geo_.bh + geo_.sq_h * 0.58));
}

VideoChunkMapper::~VideoChunkMapper() {
    map_failed_ = true;
    results_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

void VideoChunkMapper::start(std::atomic<bool>* cancel_flag) {
    for (int i = 0; i < num_threads_; ++i) workers_.emplace_back(&VideoChunkMapper::map_worker, this, i, cancel_flag);
}

bool VideoChunkMapper::get_chunk_results(int chunk_idx, std::vector<CandidateFrame>& out_candidates, std::atomic<bool>* cancel_flag) {
    std::unique_lock<std::mutex> lock(results_mutex_);
    while (!chunk_done_[chunk_idx]) {
        if ((cancel_flag && *cancel_flag) || map_failed_.load()) return false;
        results_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    out_candidates = std::move(chunk_results_[chunk_idx]);
    return true;
}

bool VideoChunkMapper::peek_next_chunk_front(int chunk_idx, CandidateFrame& out_cf, std::atomic<bool>* cancel_flag) {
    std::unique_lock<std::mutex> lock(results_mutex_);
    while (!chunk_done_[chunk_idx]) {
        if ((cancel_flag && *cancel_flag) || map_failed_.load()) return false;
        results_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (!chunk_results_[chunk_idx].empty()) {
        out_cf = chunk_results_[chunk_idx].front();
        return true;
    }
    return false;
}

void VideoChunkMapper::consume_next_chunk_front(int chunk_idx) {
    std::unique_lock<std::mutex> lock(results_mutex_);
    if (!chunk_results_[chunk_idx].empty()) chunk_results_[chunk_idx].erase(chunk_results_[chunk_idx].begin());
}

void VideoChunkMapper::map_worker(int worker_idx, std::atomic<bool>* cancel_flag) {
    std::this_thread::sleep_for(std::chrono::milliseconds(worker_idx * 150));
    cv::VideoCapture cap;
    cap.open(safe_video_path_, cv::CAP_FFMPEG);
    if (!cap.isOpened()) cap.open(safe_video_path_, cv::CAP_ANY);
    if (!cap.isOpened()) { map_failed_ = true; std::lock_guard<std::mutex> lock(results_mutex_); results_cv_.notify_all(); return; }

    auto round_t = [](double val) { return std::round(val * 100.0) / 100.0; };
    constexpr double fine_step = 0.1, quiet_coarse_step = 1.0, quiet_before_coarse_scan = 2.0;

    while (true) {
        int chunk_idx = next_chunk_to_map_.fetch_add(1);
        if (chunk_idx >= total_chunks_) break;

        while (true) {
            if ((cancel_flag && *cancel_flag) || map_failed_.load()) return;
            int current_red = current_reducing_chunk_.load();
            if (chunk_idx > current_red + max_lookahead_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            else break;
        }
        if ((cancel_flag && *cancel_flag) || map_failed_.load()) break;

        double start_t = chunk_idx * chunk_duration_;
        double end_t = std::min(duration_, start_t + chunk_duration_);
        std::vector<CandidateFrame> local_candidates;

        double current_msec = cap.get(cv::CAP_PROP_POS_MSEC);
        double v_fps = cap.get(cv::CAP_PROP_FPS);
        if (v_fps <= 0) v_fps = 30.0;

        if (std::abs(current_msec - start_t * 1000.0) > 100.0) {
            cap.set(cv::CAP_PROP_POS_MSEC, start_t * 1000.0);
            int max_grabs = static_cast<int>(v_fps * std::max(600.0, start_t + 60.0)), grabs = 0;
            cap.grab();
            if (cap.get(cv::CAP_PROP_POS_MSEC) <= 0.01 && start_t > 1.0) {
                double target_f = start_t * v_fps;
                while (cap.get(cv::CAP_PROP_POS_FRAMES) < target_f && grabs < max_grabs) { if ((cancel_flag && *cancel_flag) || !cap.grab()) break; grabs++; }
            } else {
                while (cap.get(cv::CAP_PROP_POS_MSEC) < start_t * 1000.0 && grabs < max_grabs) { if ((cancel_flag && *cancel_flag) || !cap.grab()) break; grabs++; }
            }
        }

        double local_t = cap.get(cv::CAP_PROP_POS_MSEC) / 1000.0;
        if (local_t <= 0.01 && start_t > 1.0) local_t = (cap.get(cv::CAP_PROP_POS_FRAMES) / v_fps) > 0.01 ? (cap.get(cv::CAP_PROP_POS_FRAMES) / v_fps) : start_t;

        auto emit_candidate = [&](double candidate_t, const cv::Mat& frame, const cv::Mat& bgr_view, const cv::Mat& gray) {
            CandidateFrame cf; cf.t = candidate_t;
            if (debug_level_ != 0) cf.full_bgr = frame.clone();
            if (has_clocks_) { cf.clock_top_bgr = frame(cv::Rect(roi_x1_, top_roi_y1_, roi_x2_ - roi_x1_, top_roi_y2_ - top_roi_y1_)).clone(); cf.clock_bot_bgr = frame(cv::Rect(roi_x1_, bot_roi_y1_, roi_x2_ - roi_x1_, bot_roi_y2_ - bot_roi_y1_)).clone(); }
            cf.board_bgr = bgr_view.clone(); cf.board_gray = gray.clone(); cf.board_hash = compute_all_square_means(cf.board_gray, geo_, margin_h_, margin_w_);
            local_candidates.push_back(std::move(cf)); map_candidates_emitted_.fetch_add(1, std::memory_order_relaxed);
        };

        cv::Mat frame, prev_gray, board_gray, motion_diff, thresh;
        cv::Mat prev_frame, prev_board_bgr_view;
        double prev_local_t = 0.0, last_emit_t = 0.0;
        bool emitted_initial = false, has_pending_motion = false;
        double quiet_seconds = 0.0;
        
        while (local_t < end_t && (!cancel_flag || !*cancel_flag)) {
            if (!cap.read(frame)) break;
            cv::Mat board_bgr_view = frame(cv::Rect(geo_.bx, geo_.by, geo_.bw, geo_.bh));
            cv::cvtColor(board_bgr_view, board_gray, cv::COLOR_BGR2GRAY);

            bool has_motion = true;
            bool motion_spike = false;
            if (!prev_gray.empty()) {
                cv::absdiff(board_gray, prev_gray, motion_diff); cv::threshold(motion_diff, thresh, 25.0, 255, cv::THRESH_BINARY);
                int changed_pixels = cv::countNonZero(thresh);
                if (changed_pixels < static_cast<int>(board_gray.total() * 0.005)) {
                    has_motion = false;
                } else if (changed_pixels > static_cast<int>(board_gray.total() * 0.045)) {
                    // A >4.5% instant shift is massive (highlights fading/appearing equals 4 squares or 6.25%).
                    // This is a UI snap/premove, not a smooth sliding piece.
                    motion_spike = true;
                }
            }

            if (!emitted_initial) { 
                emit_candidate(local_t, frame, board_bgr_view, board_gray); 
                emitted_initial = true; 
                last_emit_t = local_t;
            }
            else if (motion_spike && has_pending_motion && prev_local_t > 0.0) {
                // Back-to-back bullet moves detected. Instantly emit the frame from *before* the spike 
                // so the reducer has a chance to see the settled state of the first move!
                if (prev_local_t > last_emit_t + 0.02) {
                    emit_candidate(prev_local_t, prev_frame, prev_board_bgr_view, prev_gray);
                    last_emit_t = prev_local_t;
                }
                has_pending_motion = true;
            }
            else if (has_motion) { 
                has_pending_motion = true; 
                // Cap burst durations to 0.3s so slow drags don't hide mid-animation states indefinitely.
                if (local_t - last_emit_t > 0.3) {
                    emit_candidate(local_t, frame, board_bgr_view, board_gray);
                    last_emit_t = local_t;
                }
            }
            else if (has_pending_motion) { 
                emit_candidate(local_t, frame, board_bgr_view, board_gray); 
                has_pending_motion = false; 
                last_emit_t = local_t;
            }

            board_gray.copyTo(prev_gray);
            frame.copyTo(prev_frame);
            board_bgr_view.copyTo(prev_board_bgr_view);
            prev_local_t = local_t;

            double scan_step = (quiet_seconds >= quiet_before_coarse_scan && !has_pending_motion) ? quiet_coarse_step : fine_step;
            int frame_skip = std::max(0, static_cast<int>(std::round(v_fps * scan_step)) - 1);
            for (int j = 0; j < frame_skip; ++j) { if ((cancel_flag && *cancel_flag) || !cap.grab()) break; }
            if (has_motion) quiet_seconds = 0.0; else quiet_seconds += scan_step;
            local_t = round_t(local_t + scan_step);
        }
        std::lock_guard<std::mutex> lock(results_mutex_);
        chunk_results_[chunk_idx] = std::move(local_candidates);
        chunk_done_[chunk_idx] = true;
        results_cv_.notify_all();
    }
}
} // namespace cta
