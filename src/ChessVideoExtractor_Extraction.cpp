// Extracted from cpp directory
#include "ChessVideoExtractor.h"
#include "ChessVideoExtractor_Internal.h"
#include "BoardAnalysis.h"
#include "GPUAccelerator.h"
#include "BoardLocalizer.h"
#include "ExtractorUtils.h"
#include "MoveValidations.h"
#include "RevertManager.h"
#include "VideoChunkMapper.h"
#include "BoardCache.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace cta {

// ── Main extraction loop ────────────────────────────────────────────────────

GameData ChessVideoExtractor::extract_moves_from_video(const std::string& video_path,
                                                        const std::string& debug_label,
                                                        std::atomic<bool>* cancel_flag) {
    auto t_start = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    };

    auto log_info = [&](const std::string& msg, int percent = -1) {
        if (progress_callback_) {
            progress_callback_(percent, msg);
        } else {
            std::cout << msg << std::endl;
        }
    };

    // Initialize libchess position
    pos_ptr_ = std::make_unique<libchess::Position>("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    std::string safe_video_path = utils::get_safe_path(video_path);

    cv::VideoCapture cap = extractor_detail::open_video_capture(safe_video_path);

    if (!cap.isOpened()) {
        throw std::runtime_error("Cannot open video: " + video_path);
    }

    double fps = cap.get(cv::CAP_PROP_FPS);
    double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    double duration = total_frames / fps;

    log_info(utils::ts(elapsed()) + " Locating board coordinates using template matching...");
    cv::Mat first_frame;
    cap >> first_frame;
    if (first_frame.empty()) {
        throw std::runtime_error("Cannot read first frame of video.");
    }
    
    // Free the hardware video decoder instance to save VRAM and avoid contention
    // (the map-reduce workers use their own dedicated VideoCapture instances)
    cap.release();

    geo_ = BoardCache::load_or_locate(safe_video_path, first_frame, board_template_, [&](const std::string& msg) {
        log_info(utils::ts(elapsed()) + " " + msg);
    });

    gpu_pipeline_ = std::make_unique<GPUPipeline>();

    std::string debug_dir_name = debug_label;
    if (debug_dir_name.empty()) {
        size_t slash = video_path.find_last_of("/\\");
        std::string filename = (slash == std::string::npos) ? video_path : video_path.substr(slash + 1);
        size_t dot = filename.find_last_of(".");
        debug_dir_name = (dot == std::string::npos) ? filename : filename.substr(0, dot);
    }
    for (char& c : debug_dir_name) {
        if (static_cast<unsigned char>(c) > 127 || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|' || c == '\n' || c == '\r') {
            c = '_';
        }
    }
    std::filesystem::path temp_base = std::filesystem::temp_directory_path() / "ChessTubeAnalyzer" / "debug_screenshots" / "cpp_extraction";
    std::string debug_dir = (temp_base / debug_dir_name).string();

    if (std::filesystem::exists(debug_dir)) {
        std::filesystem::remove_all(debug_dir);
    }

    if (debug_level_ != DebugLevel::None) {
        log_info(utils::ts(elapsed()) + " Generating debug screenshot for initial board...");
        std::filesystem::create_directories(debug_dir);
        cv::Mat debug_board = first_frame.clone();
        draw_board_grid(debug_board, *geo_, cv::Scalar(0, 255, 0), 2, true);
        cv::imwrite(debug_dir + "/00_initial_board_0.00s.png", debug_board);
    }

    // Set up margins for batch square mean computation
    margin_h_ = static_cast<int>(geo_->sq_h * 0.15);
    margin_w_ = static_cast<int>(geo_->sq_w * 0.15);

    // Initialize game data
    GameData data;
    data.fens.push_back(pos_ptr_->get_fen());
    data.video_fens.push_back(pos_ptr_->get_fen());
    if (fen_cb_) fen_cb_(pos_ptr_->get_fen());

    // Extract initial clocks
    clock_cache_ = std::make_unique<ClockCache>();
    ClockState init_clocks = extract_clocks(first_frame, board_template_, *geo_, clock_cache_.get());
    data.clocks.push_back({init_clocks.active_player, init_clocks.white_time, init_clocks.black_time});

    // Board image history for revert detection
    std::vector<cv::Mat> board_image_history;
    std::vector<std::vector<double>> history_hashes;
    std::unordered_map<std::uint64_t, std::vector<int>> history_hash_index;
    cv::Mat board_gray_crop;
    cv::cvtColor(first_frame(cv::Rect(geo_->bx, geo_->by, geo_->bw, geo_->bh)), board_gray_crop, cv::COLOR_BGR2GRAY);
    RevertManager revert_mgr(*geo_, margin_h_, margin_w_);
    revert_mgr.initialize(board_gray_crop, compute_all_square_means(board_gray_crop, *geo_, margin_h_, margin_w_));
    std::vector<size_t> revert_history_ply_counts{0};

    // ── Initialize zero-copy GPU pipeline ────────────────────────────────────
    // Uploads the first board grayscale to GPU. The GPU pipeline performs
    // absdiff on GPU (eliminating 2x H→D copies per frame), then downloads
    // the diff for CPU-based square means computation to maintain precision.
    gpu_pipeline_->init();
    gpu_pipeline_active_ = gpu_pipeline_->is_available();
    if (gpu_pipeline_active_) {
        log_info(utils::ts(elapsed()) + " Zero-copy GPU pipeline enabled — absdiff on GPU, CPU integral for precision");
        gpu_pipeline_->update_current(board_gray_crop);
    } else {
        log_info(utils::ts(elapsed()) + " Using CPU pipeline for frame diff computation");
    }

    log_info(utils::ts(elapsed()) + " Scanning video frames to calculate plies...");

    auto round_t = [](double val) { return std::round(val * 100.0) / 100.0; };

    // ── Profiling counters ────────────────────────────────────────────────
    int frame_count = 0;

    constexpr double fine_step = 0.1;
    constexpr double quiet_coarse_step = 1.0;
    constexpr double quiet_before_coarse_scan = 2.0;
    double t = 0.0;
    int branch_counter = 0;
    constexpr double kMinMoveScore = 35.0;
    constexpr double kWeakCoalescedMoveScore = 35.0;
    constexpr double kMinCoalescedFollowupScore = 60.0;
    constexpr double kRevertMaxSquareHashDiff = 15.0;
    constexpr double kRevertMeanHashDiff = 8.0;
    constexpr double kRevertFullImageMeanDiff = 3.0;

    auto read_env_int = [](const char* name, int fallback) {
#ifdef _WIN32
        char* env_val = nullptr;
        size_t env_len = 0;
        if (_dupenv_s(&env_val, &env_len, name) == 0 && env_val != nullptr) {
            int parsed = std::atoi(env_val);
            free(env_val);
            return parsed > 0 ? parsed : fallback;
        }
        return fallback;
#else
        const char* env_val = std::getenv(name);
        if (!env_val) return fallback;
        int parsed = std::atoi(env_val);
        return parsed > 0 ? parsed : fallback;
#endif
    };

    auto read_env_double = [](const char* name, double fallback) {
#ifdef _WIN32
        char* env_val = nullptr;
        size_t env_len = 0;
        if (_dupenv_s(&env_val, &env_len, name) == 0 && env_val != nullptr) {
            double parsed = std::atof(env_val);
            free(env_val);
            return parsed > 0.0 ? parsed : fallback;
        }
        return fallback;
#else
        const char* env_val = std::getenv(name);
        if (!env_val) return fallback;
        double parsed = std::atof(env_val);
        return parsed > 0.0 ? parsed : fallback;
#endif
    };

    double chunk_duration = std::clamp(read_env_double("CTA_CHUNK_SECONDS", 300.0), 30.0, 300.0);
    int total_chunks = std::max(1, static_cast<int>(std::ceil(duration / chunk_duration)));

    int frame_width = first_frame.cols;
    int frame_height = first_frame.rows;
    int roi_x1 = std::max(0, static_cast<int>(geo_->bx + geo_->bw * 0.76));
    int roi_x2 = std::min(frame_width, static_cast<int>(geo_->bx + geo_->bw));
    int top_roi_y1 = std::max(0, static_cast<int>(geo_->by - geo_->sq_h * 0.55));
    int top_roi_y2 = std::max(top_roi_y1 + 1, static_cast<int>(geo_->by - geo_->sq_h * 0.08));
    int bot_roi_y1 = std::min(frame_height - 1, static_cast<int>(geo_->by + geo_->bh + geo_->sq_h * 0.07));
    int bot_roi_y2 = std::min(frame_height, static_cast<int>(geo_->by + geo_->bh + geo_->sq_h * 0.40));
    bool has_clocks = (roi_x2 > roi_x1 && top_roi_y2 > top_roi_y1 && bot_roi_y2 > bot_roi_y1);

    int num_threads = std::max(1u, std::thread::hardware_concurrency());
    
    int ffmpeg_threads = read_env_int("OPENCV_FFMPEG_THREADS", 1);

    // Prevent massive thread contention from OpenCV's internal FFmpeg multi-threading.
    int max_safe_workers = std::max(1u, static_cast<unsigned int>(std::thread::hardware_concurrency()) / ffmpeg_threads);
    num_threads = std::min(num_threads, max_safe_workers);

    if (memory_limit_mb_ > 0) {
        // Bound memory (assume ~500MB overhead per active mapped chunk + VideoCapture context)
        int max_threads_mem = std::max(1, memory_limit_mb_ / 500);
        num_threads = std::min(num_threads, max_threads_mem);
    }
    num_threads = std::min(num_threads, total_chunks);
    long long reducer_candidates_seen = 0;
    long long revert_hash_tests = 0;
    long long revert_full_tests = 0;
    long long revert_index_queries = 0;
    long long revert_index_fallbacks = 0;
    long long score_calls = 0;
    long long clock_activity_calls = 0;
    long long clock_ocr_calls = 0;
    long long revert_us = 0;
    long long score_us = 0;
    long long clock_activity_us = 0;
    long long clock_ocr_us = 0;
    bool exhaustive_revert_fallback = read_env_int("CTA_REVERT_EXHAUSTIVE_FALLBACK", 0) > 0;
    int revert_generation = 0;

    // Static block allocation prevents workers from hopping around the video and
    // destroying FFmpeg's sequential read efficiency by constantly seeking.
    int max_lookahead = read_env_int("CTA_MAX_CHUNK_LOOKAHEAD", std::max(12, num_threads * 3));

    std::ostringstream schedule_ss;
    schedule_ss << utils::ts(elapsed()) << " Launching Map-Reduce visual extraction ("
                << num_threads << " workers, " << total_chunks
                << " chunks, chunk=" << chunk_duration
                << "s, lookahead=" << max_lookahead << ")";
    log_info(schedule_ss.str());

    std::vector<bool> unresolved_consumed_squares(64, false);

    VideoChunkMapper mapper(safe_video_path, duration, chunk_duration, total_chunks,
                            *geo_, margin_h_, margin_w_, static_cast<int>(debug_level_),
                            has_clocks, max_lookahead, num_threads, frame_width, frame_height);
    mapper.start(cancel_flag);

    double last_progress_t = -1.0;
    
    for (int current_chunk = 0; current_chunk < total_chunks; ++current_chunk) {
        mapper.set_current_reducing_chunk(current_chunk);
        std::vector<CandidateFrame> candidates;
        
        if (!mapper.get_chunk_results(current_chunk, candidates, cancel_flag)) {
            if (cancel_flag && *cancel_flag) {
                log_info("\nExtraction cancelled by user.");
                break;
            }
            if (mapper.has_failed()) {
                throw std::runtime_error("Worker thread failed to open VideoCapture for a chunk.");
            }
            break;
        }

        double pct = (double)(current_chunk) / total_chunks * 100.0;
        if (pct - last_progress_t >= 1.0) {
            if (progress_callback_) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Reducing chunk %d/%d | plies: %zu", current_chunk + 1, total_chunks, data.moves.size());
                progress_callback_(static_cast<int>(pct), std::string(buf));
            } else {
                std::cout << "\r  [" << std::fixed << std::setprecision(1) << pct << "%] Reducing chunk " 
                          << current_chunk + 1 << "/" << total_chunks << "  |  plies: " << data.moves.size() << std::flush;
            }
            last_progress_t = pct;
        }

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (cancel_flag && *cancel_flag) break;
            
            auto& cf = candidates[i];
            ++reducer_candidates_seen;
            t = cf.t;
            double next_t = round_t(t + fine_step);
            ++frame_count;

        cv::Mat& full_bgr = cf.full_bgr;
        cv::Mat& board_bgr = cf.board_bgr;
        cv::Mat& board_gray = cf.board_gray;
        const cv::Mat& prev_gray = revert_mgr.get_latest_gray();

        std::vector<double> sq_means;
        double max_sd = 0;
        static thread_local cv::Mat diff;

        // Compute the accurate diff against the anchored pristine snapshot (prev_gray),
        // which is essential for correct move scoring and rejecting partial animations.
        GPUAccelerator::absdiff(board_gray, prev_gray, diff);
        sq_means = compute_all_square_means(diff, *geo_, margin_h_, margin_w_);
        for (double sd : sq_means) {
            if (sd > max_sd) max_sd = sd;
        }

        if (max_sd < 15.0) {
            continue;
        }

        std::vector<double> current_hash;
        bool hash_computed = false;

        // O(1) Perceptual Hash Revert Detection
        if (revert_mgr.history_size() > 1) {
                auto revert_start = std::chrono::steady_clock::now();
                current_hash = cf.board_hash.empty()
                    ? compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_)
                    : cf.board_hash;
                hash_computed = true;

                int best_history_idx = revert_mgr.find_revert_idx(board_gray, current_hash,
                                                                   revert_hash_tests, revert_full_tests,
                                                                   revert_index_queries, revert_index_fallbacks,
                                                                   exhaustive_revert_fallback);

                revert_us += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - revert_start).count();

                // A mean pixel difference < 3.0 indicates a near-identical board state.
                if (best_history_idx >= 0 && best_history_idx < static_cast<int>(revert_history_ply_counts.size())) {
                    size_t best_ply = revert_history_ply_counts[best_history_idx];
                    ++branch_counter;
                    int reverted_count = static_cast<int>(data.moves.size() - best_ply);
                    log_info("\n" + utils::ts(elapsed()) + " --- ANALYSIS REVERT at " + std::to_string(t) + "s (board matched past state) ---");
                    log_info(utils::ts(elapsed()) + " Snapped back to ply " + std::to_string(best_ply) + " (Branch " + std::to_string(branch_counter) + ")");
                    if (reverted_count > 0) {
                        log_info(utils::ts(elapsed()) + "   Saving " + std::to_string(reverted_count) + " analysis plies as a variation.");

                        VariationData var_data;
                        var_data.moves.assign(data.moves.begin() + best_ply, data.moves.end());
                        var_data.timestamps.assign(data.timestamps.begin() + best_ply, data.timestamps.end());
                        var_data.fens.assign(data.fens.begin() + best_ply, data.fens.end() - 1);
                        var_data.clocks.assign(data.clocks.begin() + best_ply + 1, data.clocks.end());
                        data.variations[best_ply].push_back(std::move(var_data));
                    }

                    data.moves.resize(best_ply);
                    data.timestamps.resize(best_ply);
                    data.fens.resize(best_ply + 1);
                    data.clocks.resize(best_ply + 1);
                    revert_mgr.resize_history(best_history_idx + 1);
                    revert_history_ply_counts.resize(best_history_idx + 1);

                    // Rebuild libchess position from the correct FEN
                    pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());

                    data.video_timestamps.push_back(t);
                    data.video_fens.push_back(data.fens.back());
                    data.video_moves.push_back("REVERT");

                    std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);

                    continue;
                }
        }

        bool extracted_in_frame = false;
        bool all_validations_passed = true;
        std::vector<bool> consumed_squares = unresolved_consumed_squares;

        // Loop to extract potentially multiple overlapping moves from a single coalesced frame
        while (true) {
            // Apply consumed_squares to the current sq_means just in case
            for (int sq = 0; sq < 64; ++sq) {
                if (consumed_squares[sq]) sq_means[sq] = 0.0;
            }

            // Score moves using libchess legal move generation
            auto score_start = std::chrono::steady_clock::now();
            auto best = this->score_moves_for_board(sq_means);
            ++score_calls;
            score_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - score_start).count();
                
            double min_move_score = extracted_in_frame ? 15.0 : kMinMoveScore;
            if (best.score <= min_move_score || best.from_sq < 0) {
                break; // No more moves in this frame
            }
            if (extracted_in_frame && (consumed_squares[best.from_sq] || consumed_squares[best.to_sq])) {
                break;
            }

            // ── Move settling: peek ahead to confirm the move has settled ──
            // Always peek ahead to capture the peak of the animation and prevent ghost diffs.
            constexpr double kMaxSettleWindowSeconds = 0.75;
            const double settle_start_t = t;
            while (true) {
                bool found_settle = false;
                CandidateFrame settle_cf;
                
                if (i + 1 < candidates.size()) {
                    settle_cf = candidates[i + 1];
                    found_settle = true;
                } else if (current_chunk + 1 < total_chunks) {
                    if (mapper.peek_next_chunk_front(current_chunk + 1, settle_cf, cancel_flag)) {
                        found_settle = true;
                    }
                }

                if (found_settle) {
                    if (settle_cf.t - settle_start_t > kMaxSettleWindowSeconds) {
                        break;
                    }

                    static thread_local cv::Mat settle_diff;
                    GPUAccelerator::absdiff(settle_cf.board_gray, revert_mgr.get_latest_gray(), settle_diff);
                    auto settle_sq_means = compute_all_square_means(settle_diff, *geo_, margin_h_, margin_w_);
                        for (int sq = 0; sq < 64; ++sq) {
                            if (consumed_squares[sq]) settle_sq_means[sq] = 0.0;
                        }
                    auto settle_score_start = std::chrono::steady_clock::now();
                    auto settle_best_tmp = this->score_moves_for_board(settle_sq_means);
                    ++score_calls;
                    score_us += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - settle_score_start).count();

                    if (settle_best_tmp.score > min_move_score && settle_best_tmp.from_sq >= 0) {
                        
                        // Check for unrelated motion relative to BOTH the current best and new candidate.
                        // This prevents fading trails of the current move from triggering unrelated motion,
                        // while still catching completely separate overlapping moves (e.g. opponent premove).
                        bool unrelated_motion = false;
                        for (int sq = 0; sq < 64; ++sq) {
                            if (settle_sq_means[sq] > 25.0) { // Tolerate large piece shadow/animation spillover
                                if (sq == settle_best_tmp.from_sq || sq == settle_best_tmp.to_sq) continue;
                                if (sq == best.from_sq || sq == best.to_sq) continue;
                                
                                // Ignore castling rook squares
                                if (settle_best_tmp.from_sq == 4 && settle_best_tmp.to_sq == 6 && (sq == 7 || sq == 5)) continue;
                                if (settle_best_tmp.from_sq == 4 && settle_best_tmp.to_sq == 2 && (sq == 0 || sq == 3)) continue;
                                if (settle_best_tmp.from_sq == 60 && settle_best_tmp.to_sq == 62 && (sq == 63 || sq == 61)) continue;
                                if (settle_best_tmp.from_sq == 60 && settle_best_tmp.to_sq == 58 && (sq == 56 || sq == 59)) continue;
                                if (best.from_sq == 4 && best.to_sq == 6 && (sq == 7 || sq == 5)) continue;
                                if (best.from_sq == 4 && best.to_sq == 2 && (sq == 0 || sq == 3)) continue;
                                if (best.from_sq == 60 && best.to_sq == 62 && (sq == 63 || sq == 61)) continue;
                                if (best.from_sq == 60 && best.to_sq == 58 && (sq == 56 || sq == 59)) continue;
                                
                                // Ignore en passant capture square
                                if (((settle_best_tmp.to_sq & 7) | (settle_best_tmp.from_sq & 0x38)) == sq) continue;
                                if (((best.to_sq & 7) | (best.from_sq & 0x38)) == sq) continue;
                                
                                unrelated_motion = true;
                                break;
                            }
                        }
                        
                        if (unrelated_motion) {
                            break; // Stop settling, next move is overlapping
                        }

                        bool is_same_move = (settle_best_tmp.from_sq == best.from_sq && settle_best_tmp.to_sq == best.to_sq);

                        bool can_evolve = false;
                        if (!is_same_move && settle_best_tmp.score > best.score) {
                            const char* current_from = utils::sq_name(best.from_sq);
                            const char* current_to = utils::sq_name(best.to_sq);
                            bool current_move_is_registered = extractor_detail::passes_yellowness_check(board_bgr, *geo_, current_from, current_to);
                            bool allow_registered_retarget = false;
                            if (current_move_is_registered && settle_best_tmp.from_sq == best.from_sq) {
                                std::array<char, 64> settle_board_map = utils::expand_fen(pos_ptr_->get_fen());
                                char moving_piece = static_cast<char>(std::tolower(static_cast<unsigned char>(settle_board_map[best.from_sq])));
                                if (moving_piece != 'b' && moving_piece != 'r' && moving_piece != 'q') {
                                    allow_registered_retarget = true;
                                } else {
                                    const char* s_to = utils::sq_name(settle_best_tmp.to_sq);
                                    double current_y_to = validation::check_yellowness(board_bgr, *geo_, current_to);
                                    double settle_y_from = validation::check_yellowness(settle_cf.board_bgr, *geo_, current_from);
                                    double settle_y_to = validation::check_yellowness(settle_cf.board_bgr, *geo_, s_to);
                                    allow_registered_retarget = settle_y_from >= 25.0
                                        && settle_y_to >= current_y_to + 12.0
                                        && (settle_y_from + settle_y_to) >= 70.0;
                                }
                            }
                            if (!current_move_is_registered || allow_registered_retarget) {
                                const char* s_from = utils::sq_name(settle_best_tmp.from_sq);
                                const char* s_to = utils::sq_name(settle_best_tmp.to_sq);
                                char s_uci[6];
                                s_uci[0] = s_from[0]; s_uci[1] = s_from[1];
                                s_uci[2] = s_to[0];   s_uci[3] = s_to[1];
                                if (settle_best_tmp.promotion != '\0') {
                                    s_uci[4] = settle_best_tmp.promotion;
                                    s_uci[5] = '\0';
                                } else {
                                    s_uci[4] = '\0';
                                }
                                try {
                                    (void)pos_ptr_->parse_move(s_uci); // Throws if illegal for CURRENT player turn
                                    can_evolve = true;
                                } catch (...) {
                                    can_evolve = false;
                                }
                            }
                        }

                        // If the new candidate is the exact same move, update to capture the very end of the animation.
                        // If it's a DIFFERENT move, only switch if its score is strictly higher AND it is a legal
                        // move for the current player. This allows transient mid-air hallucinations to evolve into
                        // the true landing square, while safely rejecting overlapping opponent premoves.
                        if (is_same_move || can_evolve) {
                            t = settle_cf.t;
                            board_gray = settle_cf.board_gray;
                            board_bgr = settle_cf.board_bgr;
                            full_bgr = settle_cf.full_bgr;
                            cf.clock_top_bgr = settle_cf.clock_top_bgr;
                            cf.clock_bot_bgr = settle_cf.clock_bot_bgr;
                            cf.board_hash = settle_cf.board_hash;
                            current_hash = settle_cf.board_hash;
                            hash_computed = !current_hash.empty();
                            diff = settle_diff;
                            sq_means = settle_sq_means;
                            best = settle_best_tmp;
                            
                            // Consume the settle frame directly
                            if (i + 1 < candidates.size()) {
                                i++;
                            } else {
                                mapper.consume_next_chunk_front(current_chunk + 1);
                            }
                            continue; // Peek ahead again
                        }
                    }
                }
                break; // No better settle frame found
            }

            if (extracted_in_frame && best.score < kMinCoalescedFollowupScore) {
                std::array<char, 64> board_map = utils::expand_fen(pos_ptr_->get_fen());
                char moving_piece = board_map[best.from_sq];
                if (std::tolower(static_cast<unsigned char>(moving_piece)) == 'p') {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: stopping coalesced extraction before weak pawn follow-up move");
                    }
                    break;
                }
            }
            if (extracted_in_frame && (consumed_squares[best.from_sq] || consumed_squares[best.to_sq])) {
                break;
            }

            const char* from_name = utils::sq_name(best.from_sq);
            const char* to_name = utils::sq_name(best.to_sq);

            std::array<char, 64> current_board_map = utils::expand_fen(pos_ptr_->get_fen());
            char moving_piece = current_board_map[best.from_sq];
            if (std::tolower(static_cast<unsigned char>(moving_piece)) == 'r') {
                extractor_detail::adjust_rook_target(best.to_sq, to_name, best.from_sq, from_name, board_bgr, *geo_, pos_ptr_.get());
            }

            // Build UCI strings only when needed (avoid allocation in scoring loop)
            char move_uci_buf[6];
            move_uci_buf[0] = from_name[0]; move_uci_buf[1] = from_name[1];
            move_uci_buf[2] = to_name[0];   move_uci_buf[3] = to_name[1];
            if (best.promotion != '\0') {
                move_uci_buf[4] = best.promotion;
                move_uci_buf[5] = '\0';
            } else {
                move_uci_buf[4] = '\0';
            }
            std::string move_uci(move_uci_buf);

            // If it's a promotion, visually classify the piece type on the destination square post-settling
            if (best.promotion != '\0') {
                best.promotion = classify_promoted_piece(board_bgr, *geo_, to_name);
                move_uci.back() = best.promotion; // Update the trailing 'q' with the true piece
            }

            bool accepted_strong_inverse = false;
            if (extractor_detail::is_inverse_of_recent_move(data.moves, from_name, to_name)) {
                double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                bool strong_registered_inverse = best.score >= 60.0
                    && y_from >= 35.0
                    && y_to >= 35.0
                    && (y_from + y_to) >= 80.0;
                char inverse_piece = static_cast<char>(std::tolower(static_cast<unsigned char>(moving_piece)));
                bool sliding_inverse = inverse_piece == 'b' || inverse_piece == 'r' || inverse_piece == 'q';
                bool immediate_after_previous_move = !data.timestamps.empty() && (t - data.timestamps.back()) < 0.35;
                if (!sliding_inverse || !strong_registered_inverse || immediate_after_previous_move) {
                    all_validations_passed = false;
                    break;
                }
                accepted_strong_inverse = true;
            }

            if (!data.moves.empty() && !data.timestamps.empty() && (t - data.timestamps.back()) < 1.0) {
                const std::string& prev_move = data.moves.back();
                if (prev_move.size() >= 4) {
                    const bool touches_prev_move =
                        (from_name[0] == prev_move[0] && from_name[1] == prev_move[1]) ||
                        (from_name[0] == prev_move[2] && from_name[1] == prev_move[3]) ||
                        (to_name[0] == prev_move[0] && to_name[1] == prev_move[1]) ||
                        (to_name[0] == prev_move[2] && to_name[1] == prev_move[3]);
                    if (touches_prev_move && move_uci != prev_move) {
                        all_validations_passed = false;
                        break;
                    }
                }
            }

            // Validate the move is legal in libchess
            libchess::Move validated_move;
            if (!extractor_detail::is_valid_libchess_move(*pos_ptr_, move_uci, validated_move)) {
                all_validations_passed = false;
                break;
            }

            // ── Validation 1: Yellow square check ────────────────────────────
            if (!extractor_detail::passes_yellowness_check(board_bgr, *geo_, from_name, to_name)) {
                if (debug_level_ != DebugLevel::None) {
                    double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                    double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                    log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Missing yellow highlights: from=" + std::to_string(std::round(y_from)) + ", to=" + std::to_string(std::round(y_to)) + ")");
                }
                all_validations_passed = false;
                break;
            }

            // ── Validation 2: Hover box rejection ────────────────────────────
            if (!scratch_) scratch_ = std::make_unique<ScratchBuffers>();
            bool hover_detected = validation::check_hover_box(board_bgr, *geo_, scratch_->white_mask, scratch_->reduced, from_name) ||
                                  validation::check_hover_box(board_bgr, *geo_, scratch_->white_mask, scratch_->reduced, to_name);
            if (hover_detected) {
                double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                bool strong_registered_move = accepted_strong_inverse ||
                                              (best.score >= 60.0 && y_from >= 40.0 && y_to >= 40.0 && (y_from + y_to) >= 90.0);
                if (!strong_registered_move) {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Piece is still mid-drag)");
                    }
                    all_validations_passed = false;
                    break;
                }
            }

            // ── Validation 3: Clock turn check ───────────────────────────────
            auto clock_activity_start = std::chrono::steady_clock::now();
            std::string active_clock_player;
            if (has_clocks && !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty()) {
                active_clock_player = detect_active_clock_from_rois(cf.clock_top_bgr, cf.clock_bot_bgr);
            } else if (debug_level_ != DebugLevel::None && !full_bgr.empty()) {
                active_clock_player = extract_clocks(full_bgr, board_template_, *geo_, nullptr).active_player;
            }
            ++clock_activity_calls;
            clock_activity_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - clock_activity_start).count();
            if (!active_clock_player.empty()) {
                std::string expected = (pos_ptr_->turn() == libchess::Side::White) ? "black" : "white";
                if (active_clock_player != expected) {
                    if (extracted_in_frame && best.score < kWeakCoalescedMoveScore) {
                        if (debug_level_ != DebugLevel::None) {
                            log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (weak coalesced move before clock flip)");
                        }
                        all_validations_passed = false;
                        break;
                    }
                    // The UI clock might lag behind the yellow highlights by a few frames (premoves/ping).
                    // Since the squares have passed the strict yellowness validation (>= 70 combined) above,
                    // the server has officially registered the move. We safely bypass the clock wait.
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " clock wait bypassed (UI clock lagging behind yellow highlights)");
                    }
                }
            }

            ClockState clocks;
            auto clock_start = std::chrono::steady_clock::now();
            std::string moved_player = (pos_ptr_->turn() == libchess::Side::White) ? "white" : "black";
            if (has_clocks && !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty()) {
                clocks = extract_clocks_for_moved_player_from_rois(
                    cf.clock_top_bgr, cf.clock_bot_bgr, moved_player, clock_cache_.get(), active_clock_player);
            } else if (debug_level_ != DebugLevel::None && !full_bgr.empty()) {
                clocks = extract_clocks(full_bgr, board_template_, *geo_, clock_cache_.get());
            }
            ++clock_ocr_calls;
            clock_ocr_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - clock_start).count();

            // ── All validations passed — accept the move ─────────────────────
            data.moves.push_back(move_uci);
            data.timestamps.push_back(t);
            data.video_timestamps.push_back(t);
            data.video_moves.push_back(move_uci);

            std::ostringstream move_log_ss;
            move_log_ss << utils::ts(elapsed()) << " [Branch " << branch_counter << "] Ply " << data.moves.size()
                        << ": detected " << move_uci << " at " << t << "s (confidence: " << round_t(extractor_detail::score_to_confidence(best.score)) << "%)";
            log_info(move_log_ss.str());
            if (debug_level_ != DebugLevel::None) { // Use a lambda to pass `this` context
                extractor_detail::log_top_candidates(sq_means, pos_ptr_.get(), log_info, elapsed());
            }

            // Apply the move in libchess to update position state
            (void)pos_ptr_->makemove(validated_move);
            extracted_in_frame = true;

            consumed_squares[best.from_sq] = true;
            consumed_squares[best.to_sq] = true;
            if (validated_move.type() == libchess::MoveType::ksc || validated_move.type() == libchess::MoveType::qsc) {
                if (best.to_sq == 6) { consumed_squares[7] = true; consumed_squares[5] = true; }
                else if (best.to_sq == 62) { consumed_squares[63] = true; consumed_squares[61] = true; }
                else if (best.to_sq == 2) { consumed_squares[0] = true; consumed_squares[3] = true; }
                else if (best.to_sq == 58) { consumed_squares[56] = true; consumed_squares[59] = true; }
            } else if (validated_move.type() == libchess::MoveType::enpassant) {
                consumed_squares[(best.to_sq & 7) | (best.from_sq & 0x38)] = true;
            }

            // Update FEN, board image history, and clock history
            data.fens.push_back(pos_ptr_->get_fen());
            data.video_fens.push_back(pos_ptr_->get_fen());
            if (fen_cb_) fen_cb_(pos_ptr_->get_fen());
            
            data.clocks.push_back({clocks.active_player, clocks.white_time, clocks.black_time});

            if (debug_level_ != DebugLevel::None) {
                char fname[80];
                snprintf(fname, sizeof(fname), "%s/%02d_b%d_%s_%.2fs.png",
                         debug_dir.c_str(), static_cast<int>(data.moves.size()),
                         branch_counter, move_uci.c_str(), t);
                cv::imwrite(fname, full_bgr);
            }
        }

        if (extracted_in_frame) {
            std::vector<double> h;
            if (hash_computed) {
                h = current_hash;
            } else if (!cf.board_hash.empty()) {
                h = cf.board_hash;
            } else {
                h = compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_);
            }
            revert_mgr.push_state(board_gray, h);
            revert_history_ply_counts.push_back(data.moves.size());
            std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);
        }
    }
    }

    // Final progress line
    if (last_progress_t >= 0) {
        if (!progress_callback_) std::cout << std::endl;
    }

    std::ostringstream perf_ss;
    perf_ss << utils::ts(elapsed()) << " Extraction perf: mapped candidates=" << mapper.get_candidates_emitted()
            << ", reduced candidates=" << reducer_candidates_seen
            << ", revert hash tests=" << revert_hash_tests
            << ", revert full tests=" << revert_full_tests
            << ", revert index queries=" << revert_index_queries
            << ", revert fallbacks=" << revert_index_fallbacks
            << ", score calls=" << score_calls << " (" << (score_us / 1000.0) << " ms)"
            << ", clock activity calls=" << clock_activity_calls << " (" << (clock_activity_us / 1000.0) << " ms)"
            << ", clock OCR calls=" << clock_ocr_calls << " (" << (clock_ocr_us / 1000.0) << " ms)"
            << ", revert scan=" << (revert_us / 1000.0) << " ms";
    log_info(perf_ss.str());

    return data;
}

} // namespace cta
