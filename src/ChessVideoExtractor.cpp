// Extracted from cpp directory
#include "ChessVideoExtractor.h"
#include "UIDetectors.h"
#include "BoardAnalysis.h"
#include "GPUAccelerator.h"
#include "BoardLocalizer.h"
#include "ExtractorUtils.h"
#include "MoveValidations.h"
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

    auto& pos = *pos_ptr_;

    // Thread-local cache to avoid expanding FEN and generating legal moves on every scoring call
    struct FenCache {
        std::string fen;
        std::array<char, 64> board_map;
        std::vector<libchess::Move> legal_moves;
    };
    static thread_local FenCache fen_cache;

    const std::string& fen = pos.get_fen();
    if (fen_cache.fen != fen) {
        fen_cache.fen = fen;
        fen_cache.board_map = utils::expand_fen(fen);
        fen_cache.legal_moves.clear();
        for (const auto& m : pos.legal_moves()) {
            fen_cache.legal_moves.push_back(m);
        }
    }
    const std::array<char, 64>& board_map = fen_cache.board_map;
    const auto& legal_moves = fen_cache.legal_moves;

    MoveScore best;
    for (const auto& move : legal_moves) {
        auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
        auto raw_to = static_cast<int>(static_cast<unsigned int>(move.to()));
        int to_sq = raw_to;

        bool is_castling = (move.type() == libchess::MoveType::ksc || move.type() == libchess::MoveType::qsc);
        if (is_castling) {
            if (from_sq == 4) {
                if (raw_to == 7 || raw_to == 6) to_sq = 6;
                else if (raw_to == 0 || raw_to == 2) to_sq = 2;
            } else if (from_sq == 60) {
                if (raw_to == 63 || raw_to == 62) to_sq = 62;
                else if (raw_to == 56 || raw_to == 58) to_sq = 58;
            }
        } else {
            if ((from_sq == 4 && (to_sq == 6 || to_sq == 2)) || 
                (from_sq == 60 && (to_sq == 62 || to_sq == 58))) {
                char p = board_map[from_sq];
                if (p == 'K' || p == 'k') {
                    is_castling = true;
                }
            }
        }

        double score = sq_diffs[from_sq] + sq_diffs[to_sq];

        if (is_castling) {
            int rook_from = -1, rook_to = -1;
            if (to_sq == 6) { rook_from = 7; rook_to = 5; }
            else if (to_sq == 62) { rook_from = 63; rook_to = 61; }
            else if (to_sq == 2) { rook_from = 0; rook_to = 3; }
            else if (to_sq == 58) { rook_from = 56; rook_to = 59; }
            
            if (rook_from != -1) score += sq_diffs[rook_from] + sq_diffs[rook_to];
        }

        // En passant: captured pawn on adjacent file, same rank as moving pawn
        if (move.type() == libchess::MoveType::enpassant) {
            int captured_pawn_sq = (to_sq & 7) | (from_sq & 0x38);
            score += sq_diffs[captured_pawn_sq];
        }

        char move_promo = '\0';
        char p = board_map[from_sq];
        if ((p == 'P' && to_sq >= 56) || (p == 'p' && to_sq <= 7)) {
            libchess::Position temp_pos = pos;
            temp_pos.makemove(move);
            std::array<char, 64> board_after = utils::expand_fen(temp_pos.get_fen());
            move_promo = static_cast<char>(std::tolower(board_after[to_sq]));
        }

        // Prefer Queen promotion if scores are equal (which they will be for all 4 promotion piece types)
        bool better_score = score > best.score;
        bool equal_score_better_promotion = false;
        if (score == best.score && score > 0) {
            if (move_promo == 'q' && best.promotion != 'q') {
                equal_score_better_promotion = true;
            }
        }

        if (better_score || equal_score_better_promotion) {
            best.from_sq = from_sq;
            best.to_sq = to_sq;
            best.score = score;
            best.promotion = move_promo;
        }
    }
    return best;
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

    // 1. Try explicit FFmpeg with specific GPU device (Hardware Accelerated)
    cv::VideoCapture cap(safe_video_path, cv::CAP_FFMPEG, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY,
        cv::CAP_PROP_HW_DEVICE, 0
    });

    // 2. Fallback to FFmpeg with automatic selection
    if (!cap.isOpened()) {
        cap.open(safe_video_path, cv::CAP_FFMPEG, {
            cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
        });
    }

    // 3. Ultimate fallback
    if (!cap.isOpened()) {
        cap.open(safe_video_path, cv::CAP_ANY, {
            cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
        });
    }

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

    std::error_code ec;
    auto fsize = std::filesystem::file_size(safe_video_path, ec);
    auto ftime = std::filesystem::last_write_time(safe_video_path, ec).time_since_epoch().count();

    // Compute a fast FNV-1a hash of the board template to detect if the user changed it
    size_t template_hash = 0;
    if (!board_template_.empty()) {
        cv::Mat continuous_tpl = board_template_.isContinuous() ? board_template_ : board_template_.clone();
        const uchar* p = continuous_tpl.ptr<uchar>(0);
        size_t len = continuous_tpl.total() * continuous_tpl.elemSize();
        size_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < len; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
        template_hash = hash;
    }

    std::string cache_key = std::filesystem::absolute(safe_video_path).string()
        + "_size=" + std::to_string(fsize)
        + "_time=" + std::to_string(ftime)
        + "_frames=" + std::to_string(static_cast<int>(total_frames))
        + "_video=" + std::to_string(first_frame.cols) + "x" + std::to_string(first_frame.rows)
        + "_template=" + std::to_string(board_template_.cols) + "x" + std::to_string(board_template_.rows)
        + "_tplhash=" + std::to_string(template_hash);
    bool loaded_from_cache = false;
    
    std::filesystem::path appdata_dir;
#ifdef _WIN32
    size_t len = 0;
    char* appdata = nullptr;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        appdata_dir = std::filesystem::path(appdata) / "ChessTubeAnalyzer";
        free(appdata);
    }
#endif
    if (appdata_dir.empty()) {
        appdata_dir = std::filesystem::temp_directory_path() / "ChessTubeAnalyzer";
    }
    std::filesystem::path cache_file = appdata_dir / "board_cache.json";
    
    if (std::filesystem::exists(cache_file)) {
        try {
            std::ifstream ifs(cache_file);
            nlohmann::json j;
            ifs >> j;
            if (j.contains(cache_key)) {
                auto& c = j[cache_key];
                int bx = c["bx"];
                int by = c["by"];
                int bw = c["bw"];
                int bh = c["bh"];

                // Validate cached geometry against current frame bounds
                if (bx >= 0 && by >= 0 && bw > 0 && bh > 0 && 
                    (bx + bw) <= first_frame.cols && (by + bh) <= first_frame.rows) {
                    geo_ = std::make_unique<BoardGeometry>();
                    geo_->bx = bx;
                    geo_->by = by;
                    geo_->bw = bw;
                    geo_->bh = bh;
                    geo_->sq_w = c["sq_w"];
                    geo_->sq_h = c["sq_h"];
                    loaded_from_cache = true;
                    log_info(utils::ts(elapsed()) + " Loaded exact board scale from cache (skipped multi-pass search).");
                } else {
                    log_info(utils::ts(elapsed()) + " Cached board geometry is out of bounds for current frame. Ignoring cache.");
                }
            }
        } catch (...) {
            // Ignore parse errors, fallback to matching
        }
    }

    if (!loaded_from_cache) {
        log_info(utils::ts(elapsed()) + " Performing multi-pass template matching to find exact board scale...");
        geo_ = std::make_unique<BoardGeometry>(locate_board(first_frame, board_template_));
        
        try {
            std::filesystem::create_directories(appdata_dir);
            nlohmann::json j;
            if (std::filesystem::exists(cache_file)) {
                std::ifstream ifs(cache_file);
                ifs >> j;
            }
            j[cache_key] = {
                {"bx", geo_->bx},
                {"by", geo_->by},
                {"bw", geo_->bw},
                {"bh", geo_->bh},
                {"sq_w", geo_->sq_w},
                {"sq_h", geo_->sq_h}
            };
            std::ofstream ofs(cache_file);
            ofs << j.dump(4);
        } catch (...) {
            // Ignore save errors
        }
    }

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
    board_image_history.push_back(board_gray_crop);
    history_hashes.push_back(compute_all_square_means(board_image_history.back(), *geo_, margin_h_, margin_w_));

    auto revert_bucket = [](double v) {
        int q = static_cast<int>(std::round(v / 8.0));
        return std::clamp(q, 0, 63);
    };

    auto pack_revert_key = [](const std::array<int, 4>& buckets) {
        return (static_cast<std::uint64_t>(buckets[0])      ) |
               (static_cast<std::uint64_t>(buckets[1]) <<  6) |
               (static_cast<std::uint64_t>(buckets[2]) << 12) |
               (static_cast<std::uint64_t>(buckets[3]) << 18);
    };

    auto quadrant_revert_buckets = [&](const std::vector<double>& hash) {
        std::array<double, 4> totals{0.0, 0.0, 0.0, 0.0};
        std::array<int, 4> counts{0, 0, 0, 0};
        for (int sq = 0; sq < 64; ++sq) {
            int row = sq / 8;
            int col = sq % 8;
            int q = (row >= 4 ? 2 : 0) + (col >= 4 ? 1 : 0);
            totals[q] += hash[sq];
            ++counts[q];
        }

        std::array<int, 4> buckets{};
        for (int q = 0; q < 4; ++q) {
            buckets[q] = revert_bucket(totals[q] / std::max(1, counts[q]));
        }
        return buckets;
    };

    auto make_revert_index_keys = [&](const std::vector<double>& hash) {
        std::vector<std::uint64_t> keys;
        keys.reserve(81);
        std::array<int, 4> center = quadrant_revert_buckets(hash);
        for (int d0 = -1; d0 <= 1; ++d0) {
            for (int d1 = -1; d1 <= 1; ++d1) {
                for (int d2 = -1; d2 <= 1; ++d2) {
                    for (int d3 = -1; d3 <= 1; ++d3) {
                        std::array<int, 4> key_buckets{
                            std::clamp(center[0] + d0, 0, 63),
                            std::clamp(center[1] + d1, 0, 63),
                            std::clamp(center[2] + d2, 0, 63),
                            std::clamp(center[3] + d3, 0, 63)
                        };
                        keys.push_back(pack_revert_key(key_buckets));
                    }
                }
            }
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        return keys;
    };

    auto exact_revert_index_key = [&](const std::vector<double>& hash) {
        return pack_revert_key(quadrant_revert_buckets(hash));
    };

    auto rebuild_history_hash_index = [&]() {
        history_hash_index.clear();
        history_hash_index.reserve(history_hashes.size() * 96 + 1);
        for (int idx = 0; idx < static_cast<int>(history_hashes.size()); ++idx) {
            for (std::uint64_t key : make_revert_index_keys(history_hashes[idx])) {
                history_hash_index[key].push_back(idx);
            }
        }
    };
    rebuild_history_hash_index();

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

    auto score_to_confidence = [](double s) {
        if (s >= 60.0) return 99.9;
        if (s <= 0.0) return 0.0;
        if (s >= 25.0) return 50.0 + ((s - 25.0) / 35.0) * 49.9;
        return (s / 25.0) * 50.0;
    };

    constexpr double fine_step = 0.2;
    constexpr double quiet_coarse_step = 1.0;
    constexpr double quiet_before_coarse_scan = 2.0;
    double t = 0.0;
    int branch_counter = 0;
    constexpr double kRevertMaxSquareHashDiff = 15.0;
    constexpr double kRevertMeanHashDiff = 8.0;
    constexpr double kRevertFullImageMeanDiff = 3.0;

    // ── Map-Reduce Chunked Processing ────────────────────────────────────────
    struct CandidateFrame {
        double t;
        cv::Mat full_bgr;
        cv::Mat clock_top_bgr;
        cv::Mat clock_bot_bgr;
        cv::Mat board_bgr;
        cv::Mat board_gray;
        std::vector<double> board_hash;
    };

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

    std::vector<std::vector<CandidateFrame>> chunk_results(total_chunks);
    std::vector<bool> chunk_done(total_chunks, false);
    std::vector<int> revert_test_generation;
    std::atomic<int> current_reducing_chunk{0};
    std::atomic<bool> map_failed{false};
    std::atomic<int> next_chunk_to_map{0};
    std::mutex results_mutex;
    std::condition_variable results_cv;
    std::atomic<long long> map_candidates_emitted{0};
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

    auto wait_for_results = [&](std::unique_lock<std::mutex>& lock, auto&& ready) {
        while (!ready()) {
            if (cancel_flag && *cancel_flag) {
                return false;
            }
            if (map_failed.load()) {
                return false;
            }
            results_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return true;
    };

    // Static block allocation prevents workers from hopping around the video and
    // destroying FFmpeg's sequential read efficiency by constantly seeking.
    int max_lookahead = read_env_int("CTA_MAX_CHUNK_LOOKAHEAD", std::max(12, num_threads * 3));

    std::ostringstream schedule_ss;
    schedule_ss << utils::ts(elapsed()) << " Launching Map-Reduce visual extraction ("
                << num_threads << " workers, " << total_chunks
                << " chunks, chunk=" << chunk_duration
                << "s, lookahead=" << max_lookahead << ")";
    log_info(schedule_ss.str());

    auto map_worker = [&](int worker_idx) {
        // Stagger thread initializations to prevent I/O thrashing when 6 decoders hit the disk at once
        std::this_thread::sleep_for(std::chrono::milliseconds(worker_idx * 150));
        
        cv::VideoCapture cap;

        // Use CPU decoding for workers. Hardware decoding (NVDEC/DXVA) incurs massive 
        // driver-level synchronization locks and multi-second delays when calling cap.set() 
        // concurrently across multiple threads.
        cap.open(safe_video_path, cv::CAP_FFMPEG);
        if (!cap.isOpened()) {
            cap.open(safe_video_path, cv::CAP_ANY);
        }
        if (!cap.isOpened()) {
            map_failed = true;
            std::lock_guard<std::mutex> lock(results_mutex);
            results_cv.notify_all();
            return;
        }

        while (true) {
            int chunk_idx = next_chunk_to_map.fetch_add(1);
            if (chunk_idx >= total_chunks) {
                break;
            }

            while (true) {
                if (cancel_flag && *cancel_flag) return;
                if (map_failed.load()) return;
                int current_red = current_reducing_chunk.load();
                if (chunk_idx > current_red + max_lookahead) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } else {
                    break;
                }
            }

            if (cancel_flag && *cancel_flag) break;
            if (map_failed.load()) break;

            double start_t = chunk_idx * chunk_duration;
            double end_t = std::min(duration, start_t + chunk_duration);

            std::vector<CandidateFrame> local_candidates;

            double current_msec = cap.get(cv::CAP_PROP_POS_MSEC);
            double v_fps = cap.get(cv::CAP_PROP_FPS);
            if (v_fps <= 0) v_fps = 30.0;

            // Seek only if we are significantly off from the start time.
            if (std::abs(current_msec - start_t * 1000.0) > 100.0) {
                cap.set(cv::CAP_PROP_POS_MSEC, start_t * 1000.0);
                
                // OpenCV seeks to the preceding keyframe. We MUST fast-forward to the exact start_t.
                // Allow massive fast-forward because screen recordings can have 10+ minute keyframe intervals.
                int max_grabs = static_cast<int>(v_fps * std::max(600.0, start_t + 60.0));
                int grabs = 0;
                
                // Force a single grab so FFmpeg updates internal timestamp properties after the seek
                cap.grab();
                double post_seek_msec = cap.get(cv::CAP_PROP_POS_MSEC);
                
                if (post_seek_msec <= 0.01 && start_t > 1.0) {
                    // Timestamps broken or keyframe is exactly at 0.0. Track by frame count.
                    double target_f = start_t * v_fps;
                    while (cap.get(cv::CAP_PROP_POS_FRAMES) < target_f && grabs < max_grabs) {
                        if (cancel_flag && *cancel_flag) break;
                        if (!cap.grab()) break;
                        grabs++;
                    }
                } else {
                    // Track by MSEC.
                    while (cap.get(cv::CAP_PROP_POS_MSEC) < start_t * 1000.0 && grabs < max_grabs) {
                        if (cancel_flag && *cancel_flag) break;
                        if (!cap.grab()) break;
                        grabs++;
                    }
                }
            }

            double local_t = cap.get(cv::CAP_PROP_POS_MSEC) / 1000.0;
            // Fallback if the video container doesn't report timestamps correctly
            if (local_t <= 0.01 && start_t > 1.0) {
                local_t = cap.get(cv::CAP_PROP_POS_FRAMES) / v_fps;
                if (local_t <= 0.01) {
                    local_t = start_t;
                }
            }
            
            auto make_candidate = [&](double candidate_t,
                                      const cv::Mat& source_frame,
                                      const cv::Mat& board_bgr_view,
                                      const cv::Mat& source_board_gray) {
                CandidateFrame cf;
                cf.t = candidate_t;
                if (debug_level_ != DebugLevel::None) {
                    cf.full_bgr = source_frame.clone();
                }
                if (has_clocks) {
                    cf.clock_top_bgr = source_frame(cv::Rect(roi_x1, top_roi_y1, roi_x2 - roi_x1, top_roi_y2 - top_roi_y1)).clone();
                    cf.clock_bot_bgr = source_frame(cv::Rect(roi_x1, bot_roi_y1, roi_x2 - roi_x1, bot_roi_y2 - bot_roi_y1)).clone();
                }
                cf.board_bgr = board_bgr_view.clone();
                cf.board_gray = source_board_gray.clone();
                cf.board_hash = compute_all_square_means(cf.board_gray, *geo_, margin_h_, margin_w_);
                return cf;
            };

            auto emit_candidate = [&](CandidateFrame&& cf) {
                local_candidates.push_back(std::move(cf));
                map_candidates_emitted.fetch_add(1, std::memory_order_relaxed);
            };

            cv::Mat frame, prev_gray, board_gray, motion_diff, thresh;
            bool emitted_initial = false;
            bool has_pending_motion = false;
            double quiet_seconds = 0.0;
            
            while (local_t < end_t && (!cancel_flag || !*cancel_flag)) {
                if (!cap.read(frame)) break;

                cv::Mat board_bgr_view = frame(cv::Rect(geo_->bx, geo_->by, geo_->bw, geo_->bh));
                cv::cvtColor(board_bgr_view, board_gray, cv::COLOR_BGR2GRAY);

                bool has_motion = true;
                if (!prev_gray.empty()) {
                    cv::absdiff(board_gray, prev_gray, motion_diff);
                    cv::threshold(motion_diff, thresh, 25.0, 255, cv::THRESH_BINARY);
                    // Ignore compression artifacts and minor UI animations.
                    // A moving chess piece covers >1.5% of the board, so 0.5% is a very safe threshold.
                    if (cv::countNonZero(thresh) < static_cast<int>(board_gray.total() * 0.005)) {
                        has_motion = false;
                    }
                }

                board_gray.copyTo(prev_gray);

                if (!emitted_initial) {
                    emit_candidate(make_candidate(local_t, frame, board_bgr_view, board_gray));
                    emitted_initial = true;
                } else if (has_motion) {
                    // Do not clone every animation frame; wait for the first quiet frame
                    // and emit that settled board state instead.
                    has_pending_motion = true;
                } else if (has_pending_motion) {
                    // Keep the first quiet frame after a burst; it is usually the settled board state.
                    emit_candidate(make_candidate(local_t, frame, board_bgr_view, board_gray));
                    has_pending_motion = false;
                }

                double scan_step = (quiet_seconds >= quiet_before_coarse_scan && !has_pending_motion)
                    ? quiet_coarse_step
                    : fine_step;
                int frame_skip = std::max(0, static_cast<int>(std::round(v_fps * scan_step)) - 1);
                for (int j = 0; j < frame_skip; ++j) {
                    if (cancel_flag && *cancel_flag) break;
                    if (!cap.grab()) break;
                }
                if (has_motion) {
                    quiet_seconds = 0.0;
                } else {
                    quiet_seconds += scan_step;
                }
                local_t = round_t(local_t + scan_step);
            }

            std::lock_guard<std::mutex> lock(results_mutex);
            chunk_results[chunk_idx] = std::move(local_candidates);
            chunk_done[chunk_idx] = true;
            results_cv.notify_all();
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < num_threads; ++i) {
        workers.emplace_back(map_worker, i);
    }
    
    // RAII joiner to ensure threads are always safely joined even if an exception is thrown
    struct ThreadJoiner {
        std::vector<std::thread>& threads;
        ~ThreadJoiner() { for (auto& t : threads) if (t.joinable()) t.join(); }
    } joiner{workers};

    double last_progress_t = -1.0;
    
    for (int current_chunk = 0; current_chunk < total_chunks; ++current_chunk) {
        current_reducing_chunk.store(current_chunk);

        std::unique_lock<std::mutex> lock(results_mutex);
        wait_for_results(lock, [&]{ return chunk_done[current_chunk]; });
        
        if (cancel_flag && *cancel_flag) {
            log_info("\nExtraction cancelled by user.");
            break;
        }
        
        if (map_failed) {
            throw std::runtime_error("Worker thread failed to open VideoCapture for a chunk.");
        }

        // Take ownership of the candidates, freeing memory for the vector once out of chunk scope
        auto candidates = std::move(chunk_results[current_chunk]);
        lock.unlock();

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
        const cv::Mat& prev_gray = board_image_history.back();

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
        if (board_image_history.size() > 1) {
                auto revert_start = std::chrono::steady_clock::now();
                int best_idx = -1;
                double best_diff_val = 1e18;
                current_hash = cf.board_hash.empty()
                    ? compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_)
                    : cf.board_hash;
                hash_computed = true;

                auto test_revert_candidate = [&](int idx) {
                    if (idx < 0 || idx >= static_cast<int>(history_hashes.size()) - 1) {
                        return;
                    }
                    ++revert_hash_tests;
                    // Fast rejection using O(1) perceptual hash
                    double max_hash_diff = 0.0;
                    double sum_hash_diff = 0.0;
                    for (int i = 0; i < 64; ++i) {
                        double d_val = std::abs(current_hash[i] - history_hashes[idx][i]);
                        sum_hash_diff += d_val;
                        if (d_val > max_hash_diff) max_hash_diff = d_val;
                        if (max_hash_diff >= kRevertMaxSquareHashDiff) break; // EARLY EXIT
                    }
                    if (max_hash_diff >= kRevertMaxSquareHashDiff) return; // Guaranteed to not match
                    if ((sum_hash_diff / 64.0) >= kRevertMeanHashDiff) return;

                    // Passed fast filter, do full verification
                    ++revert_full_tests;
                    static thread_local cv::Mat d;
                    GPUAccelerator::absdiff(board_gray, board_image_history[idx], d);
                    double mean_diff = cv::mean(d)[0];

                    if (mean_diff < best_diff_val) {
                        best_diff_val = mean_diff;
                        best_idx = idx;
                    }
                };

                auto scan_indexed_revert_candidates = [&](const std::vector<int>& indices) {
                    if (revert_test_generation.size() < history_hashes.size()) {
                        revert_test_generation.resize(history_hashes.size(), 0);
                    }
                    ++revert_generation;
                    if (revert_generation == std::numeric_limits<int>::max()) {
                        std::fill(revert_test_generation.begin(), revert_test_generation.end(), 0);
                        revert_generation = 1;
                    }
                    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
                        int idx = *it;
                        if (idx >= 0 && idx < static_cast<int>(revert_test_generation.size()) &&
                            revert_test_generation[idx] != revert_generation) {
                            revert_test_generation[idx] = revert_generation;
                            test_revert_candidate(idx);
                        }
                    }
                };

                ++revert_index_queries;
                auto indexed_it = history_hash_index.find(exact_revert_index_key(current_hash));
                if (indexed_it != history_hash_index.end()) {
                    scan_indexed_revert_candidates(indexed_it->second);
                }

                if (exhaustive_revert_fallback && (best_idx < 0 || best_diff_val >= kRevertFullImageMeanDiff)) {
                    ++revert_index_fallbacks;
                    for (int idx = static_cast<int>(history_hashes.size()) - 2; idx >= 0; --idx) {
                        test_revert_candidate(idx);
                    }
                }

                revert_us += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - revert_start).count();

                // A mean pixel difference < 3.0 indicates a near-identical board state.
                if (best_idx >= 0 && best_diff_val < kRevertFullImageMeanDiff) {
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
                    board_image_history.resize(best_idx + 1);
                    history_hashes.resize(best_idx + 1);
                    rebuild_history_hash_index();

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
                    std::unique_lock<std::mutex> nx_lock(results_mutex);
                    wait_for_results(nx_lock, [&]{ return chunk_done[current_chunk + 1]; });
                    if (cancel_flag && *cancel_flag) break;
                    if (map_failed.load()) break;
                    if (!chunk_results[current_chunk + 1].empty()) {
                        settle_cf = chunk_results[current_chunk + 1].front();
                        found_settle = true;
                    }
                }

                if (found_settle) {
                    static thread_local cv::Mat settle_diff;
                    GPUAccelerator::absdiff(settle_cf.board_gray, board_image_history.back(), settle_diff);
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
                                std::lock_guard<std::mutex> nx_lock(results_mutex);
                                chunk_results[current_chunk + 1].erase(chunk_results[current_chunk + 1].begin());
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
            // --- ADDED VISIBILITY: Print Top 3 Candidates ---
            if (debug_level_ != DebugLevel::None) {
                // Reuse already-computed square means
                const auto& sq_diffs = sq_means;

                struct Cand { std::string uci; double score; };
                std::vector<Cand> cands;
                
                std::string fen = pos_ptr_->get_fen();
                std::array<char, 64> board_map = utils::expand_fen(fen);
                
                for (const auto& m : pos_ptr_->legal_moves()) {
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
                        
                        if (r_f != -1) s += sq_diffs[r_f] + sq_diffs[r_t];
                    }
                    if (m.type() == libchess::MoveType::enpassant) {
                        s += sq_diffs[(to & 7) | (f & 0x38)];
                    }
                    std::string uci;
                    uci.reserve(5);
                    uci += utils::sq_name(f);
                    uci += utils::sq_name(to);
                    char p = board_map[f];
                    if ((p == 'P' && to >= 56) || (p == 'p' && to <= 7)) {
                        libchess::Position temp_pos = *pos_ptr_;
                        temp_pos.makemove(m);
                        std::array<char, 64> board_after = utils::expand_fen(temp_pos.get_fen());
                        uci += static_cast<char>(std::tolower(board_after[to]));
                    }
                    cands.push_back({std::move(uci), s});
                }
                
                // O(N log K) partial sort since we only care about the top 3
                size_t k = std::min<size_t>(3, cands.size());
                std::partial_sort(cands.begin(), cands.begin() + k, cands.end(), [](const Cand& a, const Cand& b){ return a.score > b.score; });

                std::ostringstream cands_ss;
                cands_ss << "    " << utils::ts(elapsed()) << " > Top candidates: ";
                for (size_t i = 0; i < k; ++i) {
                    cands_ss << cands[i].uci << " (" << round_t(score_to_confidence(cands[i].score)) << "%)   ";
                }
                log_info(cands_ss.str());
            }

            // Apply the move in libchess to update position state
            pos_ptr_->makemove(validated_move);

            // Update FEN, board image history, and clock history
            data.fens.push_back(pos_ptr_->get_fen());
            data.video_fens.push_back(pos_ptr_->get_fen());
            board_image_history.push_back(board_gray.clone());
            if (hash_computed) {
                history_hashes.push_back(current_hash);
            } else if (!cf.board_hash.empty()) {
                history_hashes.push_back(cf.board_hash);
            } else {
                history_hashes.push_back(compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_));
            }
            int history_idx = static_cast<int>(history_hashes.size()) - 1;
            for (std::uint64_t key : make_revert_index_keys(history_hashes.back())) {
                history_hash_index[key].push_back(history_idx);
            }

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
    perf_ss << utils::ts(elapsed()) << " Extraction perf: mapped candidates=" << map_candidates_emitted.load()
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
