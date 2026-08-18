#include "VideoChunkMapper.h"
#include "BoardAnalysis.h"
#include "ArrowDetector.h"
#include "ClockRecognizer.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <sstream>

namespace cta {

VideoChunkMapper::VideoChunkMapper(const std::string& safe_video_path, double duration, double chunk_duration, int total_chunks,
                                   const BoardGeometry& geo, int margin_h, int margin_w, int debug_level,
                                   bool has_clocks, int max_lookahead, int num_threads, int frame_width, int frame_height,
                                   bool retain_full_frames, double full_frame_interval_seconds,
                                   const std::string& diagnostic_frame_dir,
                                   double diagnostic_frame_start, double diagnostic_frame_end,
                                   const std::string& observation_replay_path)
    : safe_video_path_(safe_video_path), duration_(duration), chunk_duration_(chunk_duration), total_chunks_(total_chunks),
      geo_(geo), margin_h_(margin_h), margin_w_(margin_w), debug_level_(debug_level), has_clocks_(has_clocks),
      max_lookahead_(max_lookahead), num_threads_(num_threads), frame_width_(frame_width), frame_height_(frame_height),
      retain_full_frames_(retain_full_frames),
      full_frame_interval_seconds_(std::max(1.0, full_frame_interval_seconds)),
      diagnostic_frame_dir_(diagnostic_frame_dir),
      diagnostic_frame_start_(diagnostic_frame_start),
      diagnostic_frame_end_(diagnostic_frame_end),
      observation_replay_path_(observation_replay_path),
      chunk_results_(total_chunks), chunk_done_(total_chunks, false) {
    const ClockRoiBounds clock_bounds = clock_roi_bounds(
        geo_, frame_width_, frame_height_);
    roi_x1_ = clock_bounds.x1;
    roi_x2_ = clock_bounds.x2;
    top_roi_y1_ = clock_bounds.top_y1;
    top_roi_y2_ = clock_bounds.top_y2;
    bot_roi_y1_ = clock_bounds.bottom_y1;
    bot_roi_y2_ = clock_bounds.bottom_y2;
    if (!diagnostic_frame_dir_.empty()) {
        std::error_code error;
        std::filesystem::create_directories(diagnostic_frame_dir_, error);
        if (error) diagnostic_frame_dir_.clear();
    }
}

VideoChunkMapper::~VideoChunkMapper() {
    map_failed_ = true;
    results_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

std::string VideoChunkMapper::failure_reason() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return map_failure_reason_;
}

void VideoChunkMapper::fail_mapping(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (map_failure_reason_.empty()) map_failure_reason_ = reason;
        map_failed_ = true;
    }
    results_cv_.notify_all();
}

void VideoChunkMapper::start(std::atomic<bool>* cancel_flag) {
    const int worker_count = observation_replay_path_.empty() ? num_threads_ : 1;
    for (int i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&VideoChunkMapper::map_worker, this, i, cancel_flag);
    }
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
    if (!observation_replay_path_.empty()) {
        std::ifstream input(observation_replay_path_);
        if (!input.is_open()) {
            fail_mapping("Could not open observation replay trace: " + observation_replay_path_);
            return;
        }

        const std::filesystem::path trace_path(observation_replay_path_);
        auto resolve_asset = [&](const nlohmann::json& images, const char* name) {
            if (!images.contains(name) || !images.at(name).is_string()) {
                return std::filesystem::path();
            }
            std::filesystem::path asset(images.at(name).get<std::string>());
            if (asset.is_relative()) asset = trace_path.parent_path() / asset;
            return asset;
        };

        std::string line;
        std::uint64_t fallback_observation_id = 0;
        try {
            while (std::getline(input, line)) {
                if (line.empty()) continue;
                const auto observation = nlohmann::json::parse(line);
                if (!observation.is_object()) {
                    throw std::runtime_error("observation record is not an object");
                }

                CandidateFrame cf;
                cf.observation_id = observation.value("observation_id", ++fallback_observation_id);
                cf.t = observation.value("timestamp", 0.0);
                const auto mapper = observation.value("mapper", nlohmann::json::object());
                cf.mapper_chunk = mapper.value("chunk", 0u);
                cf.source_frame_index = mapper.value("source_frame_index", 0ull);
                cf.emission_reason = mapper.value("emission_reason", "replayed_observation");

                const auto images = observation.value("images", nlohmann::json::object());
                const auto frame_path = resolve_asset(images, "frame");
                const auto board_path = resolve_asset(images, "board");
                const auto clock_top_path = resolve_asset(images, "clock_top");
                const auto clock_bottom_path = resolve_asset(images, "clock_bottom");
                cf.diagnostic_frame_path = frame_path.string();
                cf.diagnostic_board_path = board_path.string();
                cf.diagnostic_clock_top_path = clock_top_path.string();
                cf.diagnostic_clock_bottom_path = clock_bottom_path.string();
                if (!frame_path.empty()) cf.full_bgr = cv::imread(frame_path.string(), cv::IMREAD_COLOR);
                if (!board_path.empty()) cf.board_bgr = cv::imread(board_path.string(), cv::IMREAD_COLOR);
                if (cf.board_bgr.empty() && !cf.full_bgr.empty()) {
                    const cv::Rect board_rect(geo_.bx, geo_.by, geo_.bw, geo_.bh);
                    if (board_rect.x >= 0 && board_rect.y >= 0 &&
                        board_rect.x + board_rect.width <= cf.full_bgr.cols &&
                        board_rect.y + board_rect.height <= cf.full_bgr.rows) {
                        cf.board_bgr = cf.full_bgr(board_rect).clone();
                    }
                }
                if (cf.board_bgr.empty()) {
                    throw std::runtime_error("observation has no readable board artifact");
                }
                cv::cvtColor(cf.board_bgr, cf.board_gray, cv::COLOR_BGR2GRAY);
                if (!clock_top_path.empty()) {
                    cf.clock_top_bgr = cv::imread(clock_top_path.string(), cv::IMREAD_COLOR);
                }
                if (!clock_bottom_path.empty()) {
                    cf.clock_bot_bgr = cv::imread(clock_bottom_path.string(), cv::IMREAD_COLOR);
                }

                const auto board = observation.value("board", nlohmann::json::object());
                if (board.contains("hash") && board.at("hash").is_array()) {
                    for (const auto& value : board.at("hash")) {
                        if (value.is_number()) cf.board_hash.push_back(value.get<double>());
                    }
                }
                if (cf.board_hash.empty()) {
                    cf.board_hash = compute_all_square_means(cf.board_gray, geo_, margin_h_, margin_w_);
                }

                const int chunk_index = std::clamp(
                    static_cast<int>(std::floor(std::max(0.0, cf.t) / chunk_duration_)),
                    0, total_chunks_ - 1);
                chunk_results_[chunk_index].push_back(std::move(cf));
                map_candidates_emitted_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& error) {
            fail_mapping("Could not load observation replay trace '" +
                         observation_replay_path_ + "': " + error.what());
            return;
        }

        for (auto& chunk : chunk_results_) {
            std::sort(chunk.begin(), chunk.end(), [](const CandidateFrame& lhs, const CandidateFrame& rhs) {
                if (lhs.t != rhs.t) return lhs.t < rhs.t;
                return lhs.observation_id < rhs.observation_id;
            });
        }
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            std::fill(chunk_done_.begin(), chunk_done_.end(), true);
        }
        results_cv_.notify_all();
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(worker_idx * 150));
    cv::VideoCapture cap;
    cap.open(safe_video_path_, cv::CAP_FFMPEG);
    if (!cap.isOpened()) cap.open(safe_video_path_, cv::CAP_ANY);
    if (!cap.isOpened()) {
        fail_mapping("Could not open video for mapper worker: " + safe_video_path_);
        return;
    }

    auto round_t = [](double val) { return std::round(val * 100.0) / 100.0; };
    constexpr double fine_step = kMapperFineScanStepSeconds;
    constexpr double quiet_coarse_step = 1.0;
    constexpr double quiet_before_coarse_scan = kMapperQuietCoarseScanDelaySeconds;

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
        double next_full_frame_t = start_t;

        double current_msec = cap.get(cv::CAP_PROP_POS_MSEC);
        double v_fps = cap.get(cv::CAP_PROP_FPS);
        if (v_fps <= 0) v_fps = 30.0;

        // A sequential worker may still be near the previous chunk boundary,
        // while a parallel worker opens a fresh decoder and seeks here.  Use
        // the same explicit boundary seek for every non-initial chunk so
        // mapper output does not depend on worker count.
        if (chunk_idx > 0 || std::abs(current_msec - start_t * 1000.0) > 100.0) {
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

        auto emit_candidate = [&](double candidate_t,
                                  const cv::Mat& frame,
                                  const cv::Mat& bgr_view,
                                  const cv::Mat& gray,
                                  const cv::Mat& predecessor_bgr_view,
                                  const char* emission_reason,
                                  std::uint64_t source_frame_index) {
            CandidateFrame cf;
            const std::uint64_t emission_index = static_cast<std::uint64_t>(local_candidates.size()) + 1;
            cf.observation_id = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_idx)) << 32) |
                                emission_index;
            cf.mapper_chunk = static_cast<std::uint32_t>(chunk_idx);
            cf.source_frame_index = source_frame_index;
            cf.emission_reason = emission_reason != nullptr ? emission_reason : "unknown";
            cf.t = candidate_t;
            const bool in_diagnostic_frame_window =
                (diagnostic_frame_start_ < 0.0 || candidate_t >= diagnostic_frame_start_) &&
                (diagnostic_frame_end_ < 0.0 || candidate_t <= diagnostic_frame_end_);
            if (has_clocks_) {
                cf.clock_top_bgr = frame(cv::Rect(
                    roi_x1_, top_roi_y1_, roi_x2_ - roi_x1_,
                    top_roi_y2_ - top_roi_y1_)).clone();
                cf.clock_bot_bgr = frame(cv::Rect(
                    roi_x1_, bot_roi_y1_, roi_x2_ - roi_x1_,
                    bot_roi_y2_ - bot_roi_y1_)).clone();
            }
            cf.board_bgr = bgr_view.clone();
            cf.board_gray = gray.clone();
            cf.board_hash = compute_all_square_means(
                cf.board_gray, geo_, margin_h_, margin_w_);
            // Geometry validation needs periodic source frames during normal
            // extraction as well as diagnostic runs. Artifact emission remains
            // bounded below, but the in-memory probe must not disappear simply
            // because a diagnostic window was not requested.
            const bool retain_full_frame = debug_level_ != 0 ||
                (retain_full_frames_ && candidate_t >= next_full_frame_t);
            if (retain_full_frame) {
                cf.full_bgr = frame.clone();
                if (retain_full_frames_ && debug_level_ == 0) {
                    next_full_frame_t = candidate_t + full_frame_interval_seconds_;
                }
                if (!diagnostic_frame_dir_.empty() && in_diagnostic_frame_window &&
                    !cf.full_bgr.empty()) {
                    std::ostringstream stem;
                    stem << "observation_" << cf.observation_id << "_"
                         << std::fixed << std::setprecision(3) << candidate_t;
                    const auto frame_path = std::filesystem::path(diagnostic_frame_dir_) /
                                            (stem.str() + "_frame.png");
                    const bool frame_written = cv::imwrite(frame_path.string(), cf.full_bgr);
                    if (frame_written) {
                        cf.diagnostic_frame_path = frame_path.string();
                    }
                }
            }
            if (!diagnostic_frame_dir_.empty() && in_diagnostic_frame_window &&
                !cf.board_bgr.empty()) {
                std::ostringstream stem;
                stem << "observation_" << cf.observation_id << "_"
                     << std::fixed << std::setprecision(3) << candidate_t;
                const auto board_path = std::filesystem::path(diagnostic_frame_dir_) /
                                        (stem.str() + "_board.png");
                const auto clock_top_path = std::filesystem::path(diagnostic_frame_dir_) /
                                            (stem.str() + "_clock_top.png");
                const auto clock_bottom_path = std::filesystem::path(diagnostic_frame_dir_) /
                                               (stem.str() + "_clock_bottom.png");
                if (cv::imwrite(board_path.string(), cf.board_bgr)) {
                    cf.diagnostic_board_path = board_path.string();
                }
                if (!predecessor_bgr_view.empty()) {
                    const auto predecessor_path = std::filesystem::path(diagnostic_frame_dir_) /
                        (stem.str() + "_predecessor_board.png");
                    if (cv::imwrite(predecessor_path.string(), predecessor_bgr_view)) {
                        cf.diagnostic_predecessor_board_path = predecessor_path.string();
                    }
                }
                if (!cf.clock_top_bgr.empty() &&
                    cv::imwrite(clock_top_path.string(), cf.clock_top_bgr)) {
                    cf.diagnostic_clock_top_path = clock_top_path.string();
                }
                if (!cf.clock_bot_bgr.empty() &&
                    cv::imwrite(clock_bottom_path.string(), cf.clock_bot_bgr)) {
                    cf.diagnostic_clock_bottom_path = clock_bottom_path.string();
                }
            }
            cf.yellow_arrows = find_yellow_arrows(frame, cv::Mat(), geo_);
            local_candidates.push_back(std::move(cf)); map_candidates_emitted_.fetch_add(1, std::memory_order_relaxed);
        };

        cv::Mat frame, prev_gray, board_gray, motion_diff, thresh;
        cv::Mat prev_frame, prev_board_bgr_view;
        double prev_local_t = 0.0, last_emit_t = 0.0;
        std::uint64_t source_frame_index = 0;
        std::uint64_t prev_source_frame_index = 0;
        bool emitted_initial = false, has_pending_motion = false;
        double quiet_after_motion_seconds = 0.0;
        double quiet_seconds = 0.0;
        
        while (local_t < end_t && (!cancel_flag || !*cancel_flag)) {
            if (!cap.read(frame)) break;
            const double decoder_frame_position = cap.get(cv::CAP_PROP_POS_FRAMES);
            source_frame_index = decoder_frame_position > 0.0
                ? static_cast<std::uint64_t>(std::llround(decoder_frame_position))
                : 0;
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
                emit_candidate(local_t, frame, board_bgr_view, board_gray,
                               cv::Mat(),
                               "initial_frame", source_frame_index);
                emitted_initial = true; 
                last_emit_t = local_t;
            }
            else if (motion_spike && has_pending_motion && prev_local_t > 0.0) {
                // Back-to-back bullet moves detected. Instantly emit the frame from *before* the spike 
                // so the reducer has a chance to see the settled state of the first move!
                if (prev_local_t > last_emit_t + 0.02) {
                    emit_candidate(prev_local_t, prev_frame, prev_board_bgr_view, prev_gray,
                                   cv::Mat(),
                                   "motion_spike_prev_frame", prev_source_frame_index);
                    last_emit_t = prev_local_t;
                }
                has_pending_motion = true;
            }
            else if (has_motion) { 
                // Preserve the leading edge of a UI update as well as its
                // settled tail.  Chess sites often paint the yellow move
                // registration on the first changed frame and remove it by
                // the time the board has become quiet; keeping only the tail
                // loses otherwise valid moves during fast analysis playback.
                if (!has_pending_motion && local_t > last_emit_t + 0.02) {
                    emit_candidate(local_t, frame, board_bgr_view, board_gray,
                                   prev_board_bgr_view,
                                   "motion_leading_edge", source_frame_index);
                    last_emit_t = local_t;
                }
                has_pending_motion = true; 
                // Cap burst durations so slow drags don't hide mid-animation
                // states indefinitely.
                if (local_t - last_emit_t > kMapperMotionBurstCapSeconds) {
                    emit_candidate(local_t, frame, board_bgr_view, board_gray,
                                   prev_board_bgr_view,
                                   "motion_burst_cap", source_frame_index);
                    last_emit_t = local_t;
                }
            }
            else if (has_pending_motion) { 
                quiet_after_motion_seconds += fine_step;
                if (quiet_after_motion_seconds >= kMapperSettleConfirmationSeconds) {
                    emit_candidate(local_t, frame, board_bgr_view, board_gray,
                                   prev_board_bgr_view,
                                   "settled_tail", source_frame_index);
                    has_pending_motion = false;
                    quiet_after_motion_seconds = 0.0;
                    last_emit_t = local_t;
                }
            }

            board_gray.copyTo(prev_gray);
            frame.copyTo(prev_frame);
            board_bgr_view.copyTo(prev_board_bgr_view);
            prev_local_t = local_t;
            prev_source_frame_index = source_frame_index;

            double scan_step = (quiet_seconds >= quiet_before_coarse_scan && !has_pending_motion) ? quiet_coarse_step : fine_step;
            int frame_skip = std::max(0, static_cast<int>(std::round(v_fps * scan_step)) - 1);
            for (int j = 0; j < frame_skip; ++j) { if ((cancel_flag && *cancel_flag) || !cap.grab()) break; }
            if (has_motion) {
                quiet_seconds = 0.0;
                quiet_after_motion_seconds = 0.0;
            } else {
                quiet_seconds += scan_step;
            }
            local_t = round_t(local_t + scan_step);
        }
        std::lock_guard<std::mutex> lock(results_mutex_);
        chunk_results_[chunk_idx] = std::move(local_candidates);
        chunk_done_[chunk_idx] = true;
        results_cv_.notify_all();
    }
}
} // namespace cta
