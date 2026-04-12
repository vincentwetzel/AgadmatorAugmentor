#include "ChessVideoExtractor.h"
#include "UIDetectors.h"
#include "FramePrefetcher.h"
#include "GPUAccelerator.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include "libchess/square.hpp"
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <string>
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

namespace aa {

struct ChessVideoExtractor::MoveScore {
    int from_sq = -1;
    int to_sq = -1;
    double score = 0.0;
};

struct ChessVideoExtractor::ScratchBuffers {
    cv::Mat white_mask;
    cv::Mat reduced;
};

// ── Utility: format elapsed seconds as [H:MM:SS.mmm] ────────────────────────
static std::string ts(double elapsed) {
    int total_ms = static_cast<int>(elapsed * 1000.0);
    int h = total_ms / 3600000;
    int m = (total_ms % 3600000) / 60000;
    int s = (total_ms % 60000) / 1000;
    int ms = total_ms % 1000;
    std::ostringstream oss;
    if (h > 0) oss << h << ":" << std::setfill('0') << std::setw(2);
    oss << m << ":" << std::setfill('0') << std::setw(2) << s << "." << std::setfill('0') << std::setw(3) << ms;
    return "[" + oss.str() + "]";
}

// ── Square helpers (convention matches libchess: a1=0, h8=63) ────────────────

// Pre-computed square name table — avoids per-call string allocation
static const char* SQ_NAMES[64] = {
    "a1","b1","c1","d1","e1","f1","g1","h1",
    "a2","b2","c2","d2","e2","f2","g2","h2",
    "a3","b3","c3","d3","e3","f3","g3","h3",
    "a4","b4","c4","d4","e4","f4","g4","h4",
    "a5","b5","c5","d5","e5","f5","g5","h5",
    "a6","b6","c6","d6","e6","f6","g6","h6",
    "a7","b7","c7","d7","e7","f7","g7","h7",
    "a8","b8","c8","d8","e8","f8","g8","h8"
};

static const char* sq_name(int idx) {
    return SQ_NAMES[idx];
}

// Pre-computes a 64-char array from a FEN string for O(1) piece lookups
static std::array<char, 64> expand_fen(const std::string& fen) {
    std::array<char, 64> board;
    board.fill(' ');
    int sq = 56; // Start at a8 (index 56)
    for (char c : fen) {
        if (c == ' ') break; // End of board layout
        if (c == '/') { sq -= 16; } // Move down a rank (e.g., from rank 8 to 7)
        else if (c >= '1' && c <= '8') { sq += (c - '0'); } // Skip empty squares
        else { board[sq++] = c; }
    }
    return board;
}

// ── Constructor ──────────────────────────────────────────────────────────────

ChessVideoExtractor::ChessVideoExtractor(const std::string& board_asset_path,
                                          const std::string& red_board_asset_path,
                                          DebugLevel debug_level)
    : debug_level_(debug_level) {
    board_template_ = cv::imread(board_asset_path);
    if (board_template_.empty()) {
        throw std::runtime_error("Could not load board asset at: " + board_asset_path);
    }

    if (!red_board_asset_path.empty()) {
        red_board_template_ = cv::imread(red_board_asset_path);
    }
}

ChessVideoExtractor::~ChessVideoExtractor() = default;

void ChessVideoExtractor::set_progress_callback(ProgressCallback cb) {
    progress_callback_ = std::move(cb);
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

    // Get legal moves from libchess
    auto legal_moves = pos.legal_moves();

    // Cache FEN for castling detection (avoid repeated get_fen() calls)
    const std::string& fen = pos.get_fen();

    // Expand FEN into an O(1) lookup array to avoid linear string scans per move
    std::array<char, 64> board_map = expand_fen(fen);

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

        if (score > best.score) {
            best.from_sq = from_sq;
            best.to_sq = to_sq;
            best.score = score;
        }
    }
    return best;
}

// ── Main extraction loop ────────────────────────────────────────────────────

GameData ChessVideoExtractor::extract_moves_from_video(const std::string& video_path,
                                                        const std::string& output_path,
                                                        const std::string& debug_label) {
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

    cv::VideoCapture cap(video_path, cv::CAP_ANY, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
    });
    if (!cap.isOpened()) {
        throw std::runtime_error("Cannot open video: " + video_path);
    }

    double fps = cap.get(cv::CAP_PROP_FPS);
    double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    double duration = total_frames / fps;

    log_info(ts(elapsed()) + " Locating board coordinates using template matching...");
    cv::Mat first_frame;
    cap >> first_frame;
    if (first_frame.empty()) {
        throw std::runtime_error("Cannot read first frame of video.");
    }
    
    // Free the hardware video decoder instance to save VRAM and avoid contention
    // (the prefetcher uses its own dedicated VideoCapture instance)
    cap.release();

    log_info(ts(elapsed()) + " Performing multi-pass template matching to find exact board scale...");
    
    // Use unique_ptr for geo, pipeline, etc. to match header
    geo_ = std::make_unique<BoardGeometry>(locate_board(first_frame, board_template_));
    gpu_pipeline_ = std::make_unique<GPUPipeline>();

    std::string debug_dir_name = debug_label;
    if (debug_dir_name.empty()) {
        std::filesystem::path p(video_path);
        debug_dir_name = p.stem().string();
    }
    std::string debug_dir = "debug_screenshots/cpp_extraction/" + debug_dir_name;

    if (std::filesystem::exists(debug_dir)) {
        std::filesystem::remove_all(debug_dir);
    }

    if (debug_level_ != DebugLevel::None) {
        log_info(ts(elapsed()) + " Generating debug screenshot for initial board...");
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

    // Extract initial clocks
    ClockState init_clocks = extract_clocks(first_frame, board_template_, *geo_);
    data.clocks.push_back({init_clocks.active_player, init_clocks.white_time, init_clocks.black_time});

    // Board image history for revert detection
    std::vector<cv::Mat> board_image_history;
    std::vector<std::vector<double>> history_hashes;
    cv::Mat first_gray;
    cv::cvtColor(first_frame, first_gray, cv::COLOR_BGR2GRAY);
    cv::Mat board_gray_crop = first_gray(cv::Rect(geo_->bx, geo_->by, geo_->bw, geo_->bh)).clone();
    board_image_history.push_back(board_gray_crop);
    history_hashes.push_back(compute_all_square_means(board_image_history.back(), *geo_, margin_h_, margin_w_));

    // ── Initialize zero-copy GPU pipeline ────────────────────────────────────
    // Uploads the first board grayscale to GPU. The GPU pipeline performs
    // absdiff on GPU (eliminating 2x H→D copies per frame), then downloads
    // the diff for CPU-based square means computation to maintain precision.
    gpu_pipeline_->init();
    gpu_pipeline_active_ = gpu_pipeline_->is_available();
    if (gpu_pipeline_active_) {
        log_info(ts(elapsed()) + " Zero-copy GPU pipeline enabled — absdiff on GPU, CPU integral for precision");
        gpu_pipeline_->update_current(board_gray_crop);
    } else {
        log_info(ts(elapsed()) + " Using CPU pipeline for frame diff computation");
    }

    log_info(ts(elapsed()) + " Scanning video frames to calculate plies...");

    auto round_t = [](double val) { return std::round(val * 100.0) / 100.0; };

    // ── Profiling counters ────────────────────────────────────────────────
    int frame_count = 0;

    auto score_to_confidence = [](double s) {
        if (s >= 60.0) return 99.9;
        if (s <= 0.0) return 0.0;
        if (s >= 25.0) return 50.0 + ((s - 25.0) / 35.0) * 49.9;
        return (s / 25.0) * 50.0;
    };

    double fine_step = 0.2;
    double t = 0.0;
    int branch_counter = 0;

    // ── Initialize frame prefetcher for async I/O hiding ─────────────────
    // The worker thread decodes frames (seek + read + crop + grayscale) in the
    // background while the main thread processes the current frame's results.
    prefetcher_ = std::make_unique<FramePrefetcher>(video_path);
    prefetcher_->init(*geo_);

    // Kick off the first frames decode (non-blocking — starts background thread)
    double last_requested_t = t;
    prefetcher_->request_next(last_requested_t, fps);
    for (int i = 1; i < 3; ++i) {
        last_requested_t = round_t(last_requested_t + fine_step);
        prefetcher_->request_next(last_requested_t, fps);
    }

    // Progress tracking
    double last_progress_t = -1.0;
    auto print_progress = [&]() {
        double pct = duration > 0 ? (t / duration) * 100.0 : 0.0;
        if (progress_callback_) {
            char buf[128];
            snprintf(buf, sizeof(buf), "t=%.1fs / %.1fs | plies: %zu | frames: %d",
                     t, duration, data.moves.size(), frame_count);
            progress_callback_(static_cast<int>(pct), std::string(buf));
        } else {
            char buf[128]; // Increased from 60 to prevent string truncation
            snprintf(buf, sizeof(buf), "  [%.1f%%] t=%.1fs / %.1fs  |  plies: %zu  |  frames scanned: %d",
                     pct, t, duration, data.moves.size(), frame_count);
            std::cout << "\r" << buf << std::flush;
        }
    };

    std::optional<FramePrefetcher::FrameData> cached_fd;

    while (t < duration) {
        FramePrefetcher::FrameData fd;
        if (cached_fd) {
            fd = std::move(*cached_fd);
            cached_fd.reset();
        } else {
            fd = prefetcher_->get_result();
            last_requested_t = round_t(last_requested_t + fine_step);
            prefetcher_->request_next(last_requested_t, fps);
        }
        if (!fd.valid) break;
        ++frame_count;

        double next_t = round_t(t + fine_step);

        // Print progress every ~1 second of video time
        if (t - last_progress_t >= 1.0) {
            print_progress();
            last_progress_t = t;
        }

        cv::Mat& full_bgr = fd.bgr;
        cv::Mat& board_bgr = fd.board_bgr;
        cv::Mat& board_gray = fd.board_gray;
        const cv::Mat& prev_gray = board_image_history.back();

        // ── Upload current board grayscale to GPU pipeline ────────────────────
        if (gpu_pipeline_active_) {
            gpu_pipeline_->update_current(board_gray);
        }

        // --- FRAME EVALUATION ---
        // GPU pipeline: GPU absdiff + GPU integral for fast change detection.
        // CPU integral recomputed only when significant diff found (64F precision).
        std::vector<double> sq_means;
        double max_sd = 0;
        cv::Mat diff;

        bool should_score = false;
        if (gpu_pipeline_active_) {
            auto gpu_means = gpu_pipeline_->compute_square_diff_means(*geo_, margin_h_, margin_w_);
            if (!gpu_means.empty()) {
                for (double sd : gpu_means) {
                    if (sd > max_sd) max_sd = sd;
                }
                if (max_sd >= 15.0) should_score = true;
            }
        }
        if (should_score || !gpu_pipeline_active_) {
            // Compute the accurate diff against the anchored pristine snapshot (prev_gray),
            // which is essential for correct move scoring and rejecting partial animations.
            GPUAccelerator::absdiff(board_gray, prev_gray, diff);
            sq_means = compute_all_square_means(diff, *geo_, margin_h_, margin_w_);
            max_sd = 0;
            for (double sd : sq_means) {
                if (sd > max_sd) max_sd = sd;
            }
        }

        if (max_sd < 15.0) {
            t = next_t;
            continue;
        }

        std::vector<double> current_hash;
        bool hash_computed = false;

        // O(1) Perceptual Hash Revert Detection
        if (board_image_history.size() > 1) {
                int best_idx = -1;
                double best_diff_val = 1e18;
                current_hash = compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_);
                hash_computed = true;

                for (int idx = static_cast<int>(history_hashes.size()) - 2; idx >= 0; --idx) {
                    // Fast rejection using O(1) perceptual hash
                    double max_hash_diff = 0.0;
                    for (int i = 0; i < 64; ++i) {
                        double d_val = std::abs(current_hash[i] - history_hashes[idx][i]);
                        if (d_val > max_hash_diff) max_hash_diff = d_val;
                        if (max_hash_diff >= 15.0) break; // EARLY EXIT
                    }
                    if (max_hash_diff >= 15.0) continue; // Guaranteed to not match

                    // Passed fast filter, do full verification
                    cv::Mat d;
                    GPUAccelerator::absdiff(board_gray, board_image_history[idx], d);

                    double sq_diff = 0.0;
                    double max_d_val = 0;
                    cv::minMaxLoc(d, nullptr, &max_d_val);
                    if (max_d_val >= 15.0) {
                        auto hist_sq_means = compute_all_square_means(d, *geo_, margin_h_, margin_w_);
                        for (double sd : hist_sq_means) {
                            if (sd > sq_diff) sq_diff = sd;
                        }
                    }

                    if (sq_diff < best_diff_val) {
                        best_diff_val = sq_diff;
                        best_idx = idx;
                    }
                }

                if (best_idx >= 0 && best_diff_val < 15.0) {
                    ++branch_counter;
                    int reverted = static_cast<int>(data.moves.size()) - best_idx;
                    log_info("\n" + ts(elapsed()) + " --- ANALYSIS REVERT at " + std::to_string(t) + "s (board matched past state) ---");
                    log_info(ts(elapsed()) + " Snapped back to ply " + std::to_string(best_idx) + " (Branch " + std::to_string(branch_counter) + ")");
                    if (reverted > 0) {
                        log_info(ts(elapsed()) + "   Rolling back " + std::to_string(reverted) + " analysis plies");
                    }

                    data.moves.resize(best_idx);
                    data.timestamps.resize(best_idx);
                    data.fens.resize(best_idx + 1);
                    data.clocks.resize(best_idx + 1);
                    board_image_history.resize(best_idx + 1);
                    history_hashes.resize(best_idx + 1);

                    // Rebuild libchess position from the correct FEN
                    pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());

                    t = next_t;
                    continue;
                }
        }

        // Score moves using libchess legal move generation
        auto best = score_moves_for_board(sq_means);
        if (best.score > 25.0 && best.from_sq >= 0) {
            const char* from_name = sq_name(best.from_sq);
            const char* to_name = sq_name(best.to_sq);

            // Build UCI strings only when needed (avoid allocation in scoring loop)
            char move_uci_buf[5];
            move_uci_buf[0] = from_name[0]; move_uci_buf[1] = from_name[1];
            move_uci_buf[2] = to_name[0];   move_uci_buf[3] = to_name[1];
            move_uci_buf[4] = '\0';
            std::string move_uci(move_uci_buf);

            // ── Move settling: peek ahead 0.2s to confirm the move has settled ──
            if (best.score < 50.0) {
                FramePrefetcher::FrameData settle_fd = prefetcher_->get_result();
                last_requested_t = round_t(last_requested_t + fine_step);
                prefetcher_->request_next(last_requested_t, fps);

                if (settle_fd.valid) {
                    cv::Mat settle_diff;
                    GPUAccelerator::absdiff(settle_fd.board_gray, board_image_history.back(), settle_diff);
                    auto settle_sq_means = compute_all_square_means(settle_diff, *geo_, margin_h_, margin_w_);
                    auto settle_best_tmp = score_moves_for_board(settle_sq_means);

                    if (settle_best_tmp.score > 25.0 && settle_best_tmp.from_sq >= 0) {
                        const char* settle_from = sq_name(settle_best_tmp.from_sq);
                        const char* settle_to = sq_name(settle_best_tmp.to_sq);
                        // Accept if same move or strictly better score
                        if (settle_best_tmp.score > best.score) {
                            t = next_t;
                            board_gray = settle_fd.board_gray;
                            board_bgr = settle_fd.board_bgr;
                            full_bgr = settle_fd.bgr;
                            diff = settle_diff;
                            best = settle_best_tmp;
                            from_name = settle_from;
                            to_name = settle_to;
                            move_uci_buf[0] = settle_from[0]; move_uci_buf[1] = settle_from[1];
                            move_uci_buf[2] = settle_to[0];   move_uci_buf[3] = settle_to[1];
                            move_uci = move_uci_buf;
                            
                            next_t = round_t(t + fine_step);
                            if (gpu_pipeline_active_) {
                                gpu_pipeline_->update_current(board_gray); // Keep GPU pipeline in sync with settle frame
                            }
                        } else {
                            cached_fd = std::move(settle_fd);
                        }
                    } else {
                        cached_fd = std::move(settle_fd);
                    }
                }
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
                t = next_t;
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
            t = next_t;
                continue;
            }

            // ── Validation 1: Yellow square check ────────────────────────────
            // board_bgr is already cropped to the board region (same as Python)
            auto is_yellow = [this, &board_bgr](const char* sq_name) {
                int col = sq_name[0] - 'a';
                int rank = sq_name[1] - '1';
                int row = 7 - rank;
                int y1 = static_cast<int>(row * geo_->sq_h);
                int y2 = static_cast<int>((row + 1) * geo_->sq_h);
                int x1 = static_cast<int>(col * geo_->sq_w);
                int x2 = static_cast<int>((col + 1) * geo_->sq_w);
                int ch = static_cast<int>(geo_->sq_h * 0.12);
                int cw = static_cast<int>(geo_->sq_w * 0.12);

                // Clamp to frame bounds
                int fh = board_bgr.rows, fw = board_bgr.cols;
                x1 = std::max(0, std::min(x1, fw - 1));
                y1 = std::max(0, std::min(y1, fh - 1));
                x2 = std::max(x1 + 1, std::min(x2, fw));
                y2 = std::max(y1 + 1, std::min(y2, fh));

                cv::Rect corners[4] = {
                    {x1, y1, std::min(cw, x2 - x1), std::min(ch, y2 - y1)},
                    {std::max(x1, x2 - cw), y1, std::min(cw, x2 - std::max(x1, x2 - cw)), std::min(ch, y2 - y1)},
                    {x1, std::max(y1, y2 - ch), std::min(cw, x2 - x1), std::min(ch, y2 - std::max(y1, y2 - ch))},
                    {std::max(x1, x2 - cw), std::max(y1, y2 - ch), std::min(cw, x2 - std::max(x1, x2 - cw)), std::min(ch, y2 - std::max(y1, y2 - ch))}
                };

                double y_score = 0;
                for (const auto& c : corners) {
                    if (c.width <= 0 || c.height <= 0) continue;
                    cv::Mat patch = board_bgr(c);
                    double sum_y = 0.0;
                    for (int r = 0; r < patch.rows; ++r) {
                        const auto* ptr = patch.ptr<cv::Vec3b>(r);
                        for (int pc = 0; pc < patch.cols; ++pc) {
                            sum_y += (ptr[pc][2] + ptr[pc][1]) / 2.0 - ptr[pc][0];
                        }
                    }
                    y_score += sum_y / (patch.rows * patch.cols);
                }
                return y_score / 4.0;
            };

            double y_from = is_yellow(from_name);
            double y_to = is_yellow(to_name);
            if (y_from < 40.0 || y_to < 40.0) {
                if (debug_level_ != DebugLevel::None) {
                    log_info("    " + ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Missing yellow highlights)");
                }
                t = next_t;
                continue;
            }

            // ── Validation 2: Hover box rejection ────────────────────────────
            auto has_hover_box = [this, &board_bgr](const char* sq_name) {
                int col = sq_name[0] - 'a';
                int rank = sq_name[1] - '1';
                int row = 7 - rank;
                int y1 = static_cast<int>(row * geo_->sq_h);
                int y2 = static_cast<int>((row + 1) * geo_->sq_h);
                int x1 = static_cast<int>(col * geo_->sq_w);
                int x2 = static_cast<int>((col + 1) * geo_->sq_w);

                // Clamp to frame bounds
                int fh = board_bgr.rows, fw = board_bgr.cols;
                x1 = std::max(0, std::min(x1, fw - 1));
                y1 = std::max(0, std::min(y1, fh - 1));
                x2 = std::max(x1 + 1, std::min(x2, fw));
                y2 = std::max(y1 + 1, std::min(y2, fh));

                // Reuse scratch white_mask buffer
                int sw = x2 - x1, sh = y2 - y1;
                if (!scratch_) scratch_ = std::make_unique<ScratchBuffers>();
                if (scratch_->white_mask.rows < sh || scratch_->white_mask.cols < sw) {
                    scratch_->white_mask = cv::Mat(sh, sw, CV_8UC1);
                }
                cv::Mat white_mask_roi = scratch_->white_mask(cv::Rect(0, 0, sw, sh));
                cv::inRange(board_bgr(cv::Rect(x1, y1, sw, sh)), cv::Scalar(160, 160, 160), cv::Scalar(255, 255, 255), white_mask_roi);

                int thickness = std::max(3, static_cast<int>(geo_->sq_w * 0.08));
                cv::Mat top = white_mask_roi(cv::Rect(0, 0, sw, thickness));
                cv::Mat bottom = white_mask_roi(cv::Rect(0, sh - thickness, sw, thickness));
                cv::Mat left = white_mask_roi(cv::Rect(0, 0, thickness, sh));
                cv::Mat right = white_mask_roi(cv::Rect(sw - thickness, 0, thickness, sh));

                cv::reduce(top, scratch_->reduced, 0, cv::REDUCE_MAX);
                double r0 = static_cast<double>(cv::countNonZero(scratch_->reduced)) / std::max(1, sw);
                cv::reduce(bottom, scratch_->reduced, 0, cv::REDUCE_MAX);
                double r1 = static_cast<double>(cv::countNonZero(scratch_->reduced)) / std::max(1, sw);
                cv::reduce(left, scratch_->reduced, 1, cv::REDUCE_MAX);
                double r2 = static_cast<double>(cv::countNonZero(scratch_->reduced)) / std::max(1, sh);
                cv::reduce(right, scratch_->reduced, 1, cv::REDUCE_MAX);
                double r3 = static_cast<double>(cv::countNonZero(scratch_->reduced)) / std::max(1, sh);

                int visible = (r0 > 0.10) + (r1 > 0.10) + (r2 > 0.10) + (r3 > 0.10);
                return visible >= 2;
            };

            if (has_hover_box(to_name)) {
                if (debug_level_ != DebugLevel::None) {
                    log_info("    " + ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Piece is still mid-drag)");
                }
                t = next_t;
                continue;
            }

            // ── Validation 3: Clock turn check ───────────────────────────────
            ClockState clocks = extract_clocks(full_bgr, board_template_, *geo_, clock_cache_.get());
            if (!clocks.active_player.empty()) {
                std::string expected = (pos_ptr_->turn() == libchess::Side::White) ? "black" : "white";
                if (clocks.active_player != expected) {
                    if (debug_level_ != DebugLevel::None)
                        log_info("    " + ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Waiting for clock to flip)");
                    t = next_t;
                    continue;
                }
            }

            // ── All validations passed — accept the move ─────────────────────
            data.moves.push_back(move_uci);
            data.timestamps.push_back(t);

            std::ostringstream move_log_ss;
            move_log_ss << ts(elapsed()) << " [Branch " << branch_counter << "] Ply " << data.moves.size()
                        << ": detected " << move_uci << " at " << t << "s (confidence: " << round_t(score_to_confidence(best.score)) << "%)";
            log_info(move_log_ss.str());
            // --- ADDED VISIBILITY: Print Top 3 Candidates ---
            if (debug_level_ != DebugLevel::None) {
                // Reuse already-computed square means
                const auto& sq_diffs = sq_means;

                struct Cand { std::string uci; double score; };
                std::vector<Cand> cands;
                
                std::string fen = pos_ptr_->get_fen();
                std::array<char, 64> board_map = expand_fen(fen);
                
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
                    uci.reserve(4);
                    uci += sq_name(f);
                    uci += sq_name(to);
                    cands.push_back({std::move(uci), s});
                }
                
                // O(N log K) partial sort since we only care about the top 3
                size_t k = std::min<size_t>(3, cands.size());
                std::partial_sort(cands.begin(), cands.begin() + k, cands.end(), [](const Cand& a, const Cand& b){ return a.score > b.score; });

                std::ostringstream cands_ss;
                cands_ss << "    " << ts(elapsed()) << " > Top candidates: ";
                for (size_t i = 0; i < k; ++i) {
                    cands_ss << cands[i].uci << " (" << round_t(score_to_confidence(cands[i].score)) << "%)   ";
                }
                log_info(cands_ss.str());
            }

            // Apply the move in libchess to update position state
            pos_ptr_->makemove(validated_move);

            // Update FEN, board image history, and clock history
            data.fens.push_back(pos_ptr_->get_fen());
            board_image_history.push_back(board_gray.clone());
            if (hash_computed) {
                history_hashes.push_back(current_hash);
            } else {
                history_hashes.push_back(compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_));
            }

            data.clocks.push_back({clocks.active_player, clocks.white_time, clocks.black_time});

            if (debug_level_ != DebugLevel::None) {
                char fname[80];
                snprintf(fname, sizeof(fname), "%s/%02d_b%d_%s_%.2fs.png",
                         debug_dir.c_str(), static_cast<int>(data.moves.size()),
                         branch_counter, move_uci.c_str(), t);
                cv::imwrite(fname, full_bgr);
            }

            t = next_t;
            continue;
        }

        t = next_t;
    }

    prefetcher_->stop();

    // Final progress line
    if (last_progress_t >= 0) {
        if (!progress_callback_) std::cout << "\n";
    }

    // Write JSON output
    double total_scan = elapsed();
    log_info(ts(total_scan) + " Writing output to " + output_path);
    nlohmann::json j;
    j["moves"] = data.moves;
    j["timestamps"] = data.timestamps;
    j["fens"] = data.fens;

    auto clocks_arr = nlohmann::json::array();
    for (const auto& c : data.clocks) {
        clocks_arr.push_back({
            {"active", c.active},
            {"white", c.white_time},
            {"black", c.black_time}
        });
    }
    j["clocks"] = clocks_arr;

    std::filesystem::path out_path(output_path);
    std::filesystem::create_directories(out_path.parent_path());

    std::ofstream ofs(output_path);
    ofs << j.dump(4) << "\n";

    return data;
}

} // namespace aa
