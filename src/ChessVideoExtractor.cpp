// Extracted from cpp directory
#include "ChessVideoExtractor.h"
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

struct ChessVideoExtractor::MoveScore {
    int from_sq = -1;
    int to_sq = -1;
    char promotion = '\0';
    double score = 0.0;
};

struct ChessVideoExtractor::ScratchBuffers {
    cv::Mat white_mask;
    cv::Mat reduced;
};

namespace {

double score_to_confidence(double s) {
    if (s >= 60.0) return 99.9;
    if (s <= 0.0) return 0.0;
    if (s >= 25.0) return 50.0 + ((s - 25.0) / 35.0) * 49.9;
    return (s / 25.0) * 50.0;
}

double round_t(double val) { return std::round(val * 100.0) / 100.0; }

void log_top_candidates(const std::vector<double>& sq_diffs,
                        libchess::Position* pos_ptr,
                        const std::function<void(const std::string&)>& log_info,
                        double elapsed) {
    struct Cand { std::string uci; double score; };
    std::vector<Cand> cands;
    
    std::string fen = pos_ptr->get_fen();
    std::array<char, 64> board_map = utils::expand_fen(fen);
    
    for (const auto& m : pos_ptr->legal_moves()) {
        int f = static_cast<int>(static_cast<unsigned int>(m.from()));
        int raw_to = static_cast<int>(static_cast<unsigned int>(m.to()));
        int to = raw_to;
        
        bool is_castling = (m.type() == libchess::MoveType::ksc || m.type() == libchess::MoveType::qsc);
        if (is_castling) {
            if (f == 4) {
                if (raw_to == 7 || raw_to == 6) to = 6;
                else if (raw_to == 0 || raw_to == 2) to = 2;
            } else if (f == 60) {
                if (raw_to == 63 || raw_to == 62) to = 62;
                else if (raw_to == 56 || raw_to == 58) to = 58;
            }
        } else {
            if ((f == 4 && (to == 6 || to == 2)) || 
                (f == 60 && (to == 62 || to == 58))) {
                char p = board_map[f];
                if (p == 'K' || p == 'k') {
                    is_castling = true;
                }
            }
        }

        double s = sq_diffs[f] + sq_diffs[to];

        if (is_castling) {
            int r_f = -1, r_t = -1;
            if (to == 6) { r_f = 7; r_t = 5; }
            else if (to == 62) { r_f = 63; r_t = 61; }
            else if (to == 2) { r_f = 0; r_t = 3; }
            else if (to == 58) { r_f = 56; r_t = 59; }
            
            // Subtract penalty to prevent false castling logging
            if (r_f != -1) s += sq_diffs[r_f] + sq_diffs[r_t] - 20.0;
        }
        if (m.type() == libchess::MoveType::enpassant) {
            s += sq_diffs[(to & 7) | (f & 0x38)] - 10.0;
        }
        std::string uci;
        uci.reserve(5);
        uci += utils::sq_name(f);
        uci += utils::sq_name(to);
        char p = board_map[f];
        if ((p == 'P' && to >= 56) || (p == 'p' && to <= 7)) {
            libchess::Position temp_pos = *pos_ptr;
            temp_pos.makemove(m);
            std::array<char, 64> board_after = utils::expand_fen(temp_pos.get_fen());
            uci += static_cast<char>(std::tolower(board_after[to]));
        }
        cands.push_back({std::move(uci), s});
    }
    
    size_t k = std::min<size_t>(3, cands.size());
    std::partial_sort(cands.begin(), cands.begin() + k, cands.end(), [](const Cand& a, const Cand& b){ return a.score > b.score; });

    std::ostringstream cands_ss;
    cands_ss << "    " << utils::ts(elapsed) << " > Top candidates: ";
    for (size_t i = 0; i < k; ++i) {
        cands_ss << cands[i].uci << " (" << round_t(score_to_confidence(cands[i].score)) << "%)   ";
    }
    log_info(cands_ss.str());
}

cv::VideoCapture open_video_capture(const std::string& safe_video_path) {
    cv::VideoCapture cap(safe_video_path, cv::CAP_FFMPEG, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY, cv::CAP_PROP_HW_DEVICE, 0});
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_FFMPEG, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_ANY, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
    return cap;
}

} // namespace

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
            std::cout << msg << "\n";
        }
    };

    // Initialize libchess position
    pos_ptr_ = std::make_unique<libchess::Position>("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    std::string safe_video_path = utils::get_safe_path(video_path);

    cv::VideoCapture cap = open_video_capture(safe_video_path);

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
    int top_roi_y1 = std::max(0, static_cast<int>(geo_->by - geo_->sq_h * 0.40));
    int top_roi_y2 = std::max(top_roi_y1 + 1, static_cast<int>(geo_->by - geo_->sq_h * 0.03));
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

                int best_idx = revert_mgr.find_revert_idx(board_gray, current_hash,
                                                          revert_hash_tests, revert_full_tests,
                                                          revert_index_queries, revert_index_fallbacks,
                                                          exhaustive_revert_fallback);

                revert_us += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - revert_start).count();

                // A mean pixel difference < 3.0 indicates a near-identical board state.
                if (best_idx >= 0) {
                    ++branch_counter;
                    int reverted_count = static_cast<int>(data.moves.size()) - best_idx;
                    log_info("\n" + utils::ts(elapsed()) + " --- ANALYSIS REVERT at " + std::to_string(t) + "s (board matched past state) ---");
                    log_info(utils::ts(elapsed()) + " Snapped back to ply " + std::to_string(best_idx) + " (Branch " + std::to_string(branch_counter) + ")");
                    if (reverted_count > 0) {
                        log_info(utils::ts(elapsed()) + "   Saving " + std::to_string(reverted_count) + " analysis plies as a variation.");

                        VariationData var_data;
                        var_data.moves.assign(data.moves.begin() + best_idx, data.moves.end());
                        var_data.timestamps.assign(data.timestamps.begin() + best_idx, data.timestamps.end());
                        var_data.fens.assign(data.fens.begin() + best_idx, data.fens.end() - 1);
                        var_data.clocks.assign(data.clocks.begin() + best_idx + 1, data.clocks.end());
                        data.variations[best_idx].push_back(std::move(var_data));
                    }

                    data.moves.resize(best_idx);
                    data.timestamps.resize(best_idx);
                    data.fens.resize(best_idx + 1);
                    data.clocks.resize(best_idx + 1);
                    revert_mgr.resize_history(best_idx + 1);

                    // Rebuild libchess position from the correct FEN
                    pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());

                    data.video_timestamps.push_back(t);
                    data.video_fens.push_back(data.fens.back());
                    data.video_moves.push_back("REVERT");

                    continue;
                }
        }

        // Score moves using libchess legal move generation
        auto score_start = std::chrono::steady_clock::now();
        auto best = score_moves_for_board(sq_means);
        ++score_calls;
        score_us += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - score_start).count();
        if (best.score > 25.0 && best.from_sq >= 0) {
            const char* from_name = utils::sq_name(best.from_sq);
            const char* to_name = utils::sq_name(best.to_sq);

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

            // ── Move settling: peek ahead 0.2s to confirm the move has settled ──
            if (best.score < 50.0) {
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
                    static thread_local cv::Mat settle_diff;
                    GPUAccelerator::absdiff(settle_cf.board_gray, revert_mgr.get_latest_gray(), settle_diff);
                    auto settle_sq_means = compute_all_square_means(settle_diff, *geo_, margin_h_, margin_w_);
                    auto settle_score_start = std::chrono::steady_clock::now();
                    auto settle_best_tmp = score_moves_for_board(settle_sq_means);
                    ++score_calls;
                    score_us += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - settle_score_start).count();

                    if (settle_best_tmp.score > 25.0 && settle_best_tmp.from_sq >= 0) {
                        const char* settle_from = utils::sq_name(settle_best_tmp.from_sq);
                        const char* settle_to = utils::sq_name(settle_best_tmp.to_sq);
                        if (settle_best_tmp.score > best.score) {
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
                            best = settle_best_tmp;
                            from_name = settle_from;
                            to_name = settle_to;
                            
                            move_uci_buf[0] = settle_from[0]; move_uci_buf[1] = settle_from[1];
                            move_uci_buf[2] = settle_to[0];   move_uci_buf[3] = settle_to[1];
                            if (settle_best_tmp.promotion != '\0') {
                                move_uci_buf[4] = settle_best_tmp.promotion;
                                move_uci_buf[5] = '\0';
                            } else {
                                move_uci_buf[4] = '\0';
                            }
                            move_uci = move_uci_buf;
                            
                            // Consume the settle frame directly
                            if (i + 1 < candidates.size()) {
                                i++;
                            } else {
                                mapper.consume_next_chunk_front(current_chunk + 1);
                            }
                        }
                    }
                }
            }

            // If it's a promotion, visually classify the piece type on the destination square post-settling
            if (best.promotion != '\0') {
                best.promotion = classify_promoted_piece(board_bgr, *geo_, to_name);
                move_uci.back() = best.promotion; // Update the trailing 'q' with the true piece
            }

            // Inverse move filter: reject if this is the reverse of a recent move
            bool inverse_recent = false;
            char reverse_uci_buf[5];
            reverse_uci_buf[0] = to_name[0]; reverse_uci_buf[1] = to_name[1];
            reverse_uci_buf[2] = from_name[0]; reverse_uci_buf[3] = from_name[1];
            reverse_uci_buf[4] = '\0';
            std::string_view reverse_uci(reverse_uci_buf, 4);
            size_t start = data.moves.size() > 4 ? data.moves.size() - 4 : 0;
            for (size_t i = start; i < data.moves.size(); ++i) {
                if (data.moves[i] == reverse_uci) { inverse_recent = true; break; }
            }
            if (inverse_recent && best.score < 70.0) {
                continue;
            }

            // Validate the move is legal in libchess
            libchess::Move validated_move;
            bool move_valid = false;
            try {
                validated_move = pos_ptr_->parse_move(move_uci);
                move_valid = true;
            } catch (...) {
                move_valid = false;
            }

            if (!move_valid) {
                continue;
            }

            // ── Validation 1: Yellow square check ────────────────────────────
            double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
            double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
            
            // Elastic threshold: allow one square to dip to 25.0 if the other is strong, requiring a combined score of 70.0
            if (y_from < 25.0 || y_to < 25.0 || (y_from + y_to) < 70.0) {
                if (debug_level_ != DebugLevel::None) {
                    log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Missing yellow highlights: from=" + std::to_string(std::round(y_from)) + ", to=" + std::to_string(std::round(y_to)) + ")");
                }
                continue;
            }

            // ── Validation 2: Hover box rejection ────────────────────────────
            if (!scratch_) scratch_ = std::make_unique<ScratchBuffers>();
            if (validation::check_hover_box(board_bgr, *geo_, scratch_->white_mask, scratch_->reduced, to_name)) {
                if (debug_level_ != DebugLevel::None) {
                    log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Piece is still mid-drag)");
                }
                continue;
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
                    if (debug_level_ != DebugLevel::None)
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Waiting for clock to flip)");
                    continue;
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
                        << ": detected " << move_uci << " at " << t << "s (confidence: " << round_t(score_to_confidence(best.score)) << "%)";
            log_info(move_log_ss.str());
            if (debug_level_ != DebugLevel::None) {
                log_top_candidates(sq_means, pos_ptr_.get(), log_info, elapsed());
            }

            // Apply the move in libchess to update position state
            pos_ptr_->makemove(validated_move);

            // Update FEN, board image history, and clock history
            data.fens.push_back(pos_ptr_->get_fen());
            data.video_fens.push_back(pos_ptr_->get_fen());
            if (fen_cb_) fen_cb_(pos_ptr_->get_fen());
            
            std::vector<double> h;
            if (hash_computed) {
                h = current_hash;
            } else if (!cf.board_hash.empty()) {
                h = cf.board_hash;
            } else {
                h = compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_);
            }
            revert_mgr.push_state(board_gray, h);

            data.clocks.push_back({clocks.active_player, clocks.white_time, clocks.black_time});

            if (debug_level_ != DebugLevel::None) {
                char fname[80];
                snprintf(fname, sizeof(fname), "%s/%02d_b%d_%s_%.2fs.png",
                         debug_dir.c_str(), static_cast<int>(data.moves.size()),
                         branch_counter, move_uci.c_str(), t);
                cv::imwrite(fname, full_bgr);
            }

            continue;
        }
        }
    }

    // Final progress line
    if (last_progress_t >= 0) {
        if (!progress_callback_) std::cout << "\n";
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
