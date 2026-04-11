#include "ChessVideoExtractor.h"
#include "UIDetectors.h"
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

namespace aa {

// ── Square helpers (convention matches libchess: a1=0, h8=63) ────────────────

static std::string sq_name(int idx) {
    int file = idx & 7;
    int rank = idx >> 3;
    char name[3];
    name[0] = 'a' + file;
    name[1] = '1' + rank;
    name[2] = '\0';
    return std::string(name);
}

static int parse_square_idx(const std::string& name) {
    if (name.size() < 2) return -1;
    int file = name[0] - 'a';
    int rank = name[1] - '1';
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return -1;
    return file | (rank << 3);
}

// Safely extracts the piece character at a given square (0-63) from a FEN string
static char get_piece_at(const std::string& fen, int sq) {
    int target_rank = 7 - (sq >> 3); // FEN string starts at rank 8 (index 0) down to rank 1 (index 7)
    int target_file = sq & 7;
    int current_rank = 0;
    int current_file = 0;
    for (char c : fen) {
        if (c == ' ') break; // End of board layout
        if (c == '/') {
            current_rank++;
            current_file = 0;
        } else if (c >= '1' && c <= '8') {
            int empty_count = c - '0';
            if (current_rank == target_rank && target_file >= current_file && target_file < current_file + empty_count) {
                return ' '; // Target square falls within this empty space
            }
            current_file += empty_count;
        } else {
            if (current_rank == target_rank && current_file == target_file) return c;
            current_file++;
        }
    }
    return ' ';
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

// ── Square diff calculation ──────────────────────────────────────────────────

cv::Mat ChessVideoExtractor::get_max_square_diff(const cv::Mat& img_a, const cv::Mat& img_b) {
    cv::Mat diff;
    cv::absdiff(img_a, img_b, diff);

    double max_val = 0;
    cv::minMaxLoc(diff, nullptr, &max_val);
    if (max_val < 15.0) return cv::Mat();

    double max_sq_diff = 0.0;
    for (const auto& sl : sq_slices_) {
        cv::Mat region = diff(cv::Rect(sl.x1, sl.y1, sl.x2 - sl.x1, sl.y2 - sl.y1));
        double sd = cv::mean(region)[0];
        if (sd > max_sq_diff) max_sq_diff = sd;
    }

    if (max_sq_diff <= 15.0) return cv::Mat();
    return diff;
}

// ── Move scoring using libchess ──────────────────────────────────────────────

ChessVideoExtractor::MoveScore ChessVideoExtractor::score_moves_for_board(const cv::Mat& diff_image) {
    if (!pos_ptr_) return {};

    auto& pos = *pos_ptr_;

    // Compute per-square diff means (our index convention matches libchess)
    std::vector<double> sq_diffs(64, 0.0);
    for (const auto& sl : sq_slices_) {
        cv::Mat region = diff_image(cv::Rect(sl.x1, sl.y1, sl.x2 - sl.x1, sl.y2 - sl.y1));
        sq_diffs[parse_square_idx(sl.name)] = cv::mean(region)[0];
    }

    // Get legal moves from libchess
    auto legal_moves = pos.legal_moves();

    std::string fen = pos.get_fen();

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
                char p = get_piece_at(fen, from_sq);
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
    // Initialize libchess position
    pos_ptr_ = std::make_unique<libchess::Position>("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) {
        throw std::runtime_error("Cannot open video: " + video_path);
    }

    double fps = cap.get(cv::CAP_PROP_FPS);
    double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    double duration = total_frames / fps;

    std::cout << "Locating board coordinates using template matching...\n";
    cv::Mat first_frame;
    cap >> first_frame;
    if (first_frame.empty()) {
        throw std::runtime_error("Cannot read first frame of video.");
    }

    std::cout << "Performing multi-pass template matching to find exact board scale...\n";
    geo_ = locate_board(first_frame, board_template_);

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
        std::cout << "Generating debug screenshot for initial board...\n";
        std::filesystem::create_directories(debug_dir);
        cv::Mat debug_board = first_frame.clone();
        draw_board_grid(debug_board, geo_, cv::Scalar(0, 255, 0), 2, true);
        cv::imwrite(debug_dir + "/00_initial_board_0.00s.png", debug_board);
    }

    // Set up square slices for efficient per-pixel diff
    margin_h_ = static_cast<int>(geo_.sq_h * 0.15);
    margin_w_ = static_cast<int>(geo_.sq_w * 0.15);

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = static_cast<int>(row * geo_.sq_h) + margin_h_;
            int y2 = static_cast<int>((row + 1) * geo_.sq_h) - margin_h_;
            int x1 = static_cast<int>(col * geo_.sq_w) + margin_w_;
            int x2 = static_cast<int>((col + 1) * geo_.sq_w) - margin_w_;
            char name[3];
            name[0] = 'a' + col;
            name[1] = '1' + (7 - row);  // row 0 = rank 8, row 7 = rank 1
            name[2] = '\0';
            sq_slices_.push_back({y1, y2, x1, x2, name});
        }
    }

    // Initialize game data
    GameData data;
    data.fens.push_back(pos_ptr_->get_fen());

    // Extract initial clocks
    ClockState init_clocks = extract_clocks(first_frame, board_template_, geo_);
    data.clocks.push_back({init_clocks.active_player, init_clocks.white_time, init_clocks.black_time});

    // Board image history for revert detection
    std::vector<cv::Mat> board_image_history;
    cv::Mat first_gray;
    cv::cvtColor(first_frame, first_gray, cv::COLOR_BGR2GRAY);
    board_image_history.push_back(first_gray(cv::Rect(geo_.bx, geo_.by, geo_.bw, geo_.bh)).clone());

    std::cout << "Scanning video frames at fixed intervals to calculate moves...\n";

    auto round_t = [](double val) { return std::round(val * 100.0) / 100.0; };

    // Convert absolute pixel diff scores to a human-readable 0-100% confidence scale.
    auto score_to_confidence = [](double s) {
        if (s >= 60.0) return 99.9; // A perfect pawn move scores ~65, which is maximum confidence
        if (s <= 0.0) return 0.0;
        if (s >= 25.0) return 50.0 + ((s - 25.0) / 35.0) * 49.9; // Scale 25-60 -> 50%-99.9%
        return (s / 25.0) * 50.0; // Scale 0-25 -> 0%-50%
    };

    int current_frame = 1;
    double fast_step = 1.0;
    double fine_step = 0.2;
    double t = 0.0;
    bool mode_fast = true;
    double fine_target_t = 0.0;

    int branch_counter = 0;

    while (t < duration) {
        // Seek to target frame
        int target_frame = static_cast<int>(t * fps);
        cv::Mat frame;

        if (target_frame > current_frame && target_frame - current_frame < fps * 3) {
            for (int i = 0; i < target_frame - current_frame - 1; ++i) {
                cap.grab();
            }
            cap >> frame;
            current_frame = target_frame + 1;
        } else if (target_frame == current_frame) {
            cap >> frame;
            ++current_frame;
        } else {
            cap.set(cv::CAP_PROP_POS_FRAMES, target_frame);
            cap >> frame;
            current_frame = target_frame + 1;
        }

        if (frame.empty()) break;

        cv::Mat board_bgr = frame(cv::Rect(geo_.bx, geo_.by, geo_.bw, geo_.bh));
        cv::Mat board_gray;
        cv::cvtColor(board_bgr, board_gray, cv::COLOR_BGR2GRAY);
        const cv::Mat& prev_gray = board_image_history.back();

        if (mode_fast) {
            auto diff_result = get_max_square_diff(board_gray, prev_gray);
            if (!diff_result.empty()) {
                fine_target_t = t;
                t = std::max(0.0, round_t(t - fast_step + fine_step));
                mode_fast = false;
            } else {
                t = round_t(t + fast_step);
            }
            continue;
        }

        // --- FINE MODE ---
        // Check for revert (board silently changed to match a past state)
        if (board_image_history.size() > 1) {
            auto diff_check = get_max_square_diff(board_gray, prev_gray);
            if (!diff_check.empty()) {
                int best_idx = -1;
                double best_diff_val = 1e18;

                for (int idx = static_cast<int>(board_image_history.size()) - 2; idx >= 0; --idx) {
                    cv::Mat d;
                    cv::absdiff(board_gray, board_image_history[idx], d);
                    
                    double sq_diff = 0.0;
                    double max_val = 0;
                    cv::minMaxLoc(d, nullptr, &max_val);
                    if (max_val >= 15.0) {
                        for (const auto& sl : sq_slices_) {
                            cv::Mat region = d(cv::Rect(sl.x1, sl.y1, sl.x2 - sl.x1, sl.y2 - sl.y1));
                            double sd = cv::mean(region)[0];
                            if (sd > sq_diff) sq_diff = sq_diff = sd;
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
                    std::cout << "\n--- ANALYSIS REVERT at " << t << "s (board matched past state) ---\n";
                    std::cout << "Snapped back to ply " << best_idx << " (Branch " << branch_counter << ")\n";
                    if (reverted > 0) {
                        std::cout << "  Rolling back " << reverted << " analysis moves\n";
                    }

                    data.moves.resize(best_idx);
                    data.timestamps.resize(best_idx);
                    data.fens.resize(best_idx + 1);
                    data.clocks.resize(best_idx + 1);
                    board_image_history.resize(best_idx + 1);

                    // Rebuild libchess position from the correct FEN
                    pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());

                    mode_fast = true;
                    t = round_t(t + fast_step);
                    continue;
                }
            }
        }

        // Compute per-square diffs
        cv::Mat diff;
        cv::absdiff(board_gray, prev_gray, diff);
        double max_sd = 0;
        for (const auto& sl : sq_slices_) {
            cv::Mat region = diff(cv::Rect(sl.x1, sl.y1, sl.x2 - sl.x1, sl.y2 - sl.y1));
            int idx = parse_square_idx(sl.name);
            double sd = cv::mean(region)[0];
            if (sd > max_sd) max_sd = sd;
        }

        if (max_sd < 15.0) {
            if (t >= fine_target_t) {
                mode_fast = true;
                t = round_t(t + fast_step);
            } else {
                t = round_t(t + fine_step);
            }
            continue;
        }

        // Score moves using libchess legal move generation
        auto best = score_moves_for_board(diff);
        if (best.score > 25.0 && best.from_sq >= 0) {
            std::string from_name = sq_name(best.from_sq);
            std::string to_name = sq_name(best.to_sq);
            std::string move_uci = from_name + to_name;

            // ── Move settling: peek ahead 0.4s to confirm the move has settled ──
            // At the moment a change is first detected, the piece may still be animating.
            // Peek ahead and accept the highest-scoring candidate (same move or different).
            double settle_t = round_t(t + fine_step * 2);  // 0.4s ahead
            int settle_frame = static_cast<int>(settle_t * fps);

            if (settle_frame < cap.get(cv::CAP_PROP_FRAME_COUNT)) {
                // Seek to settle_t
                if (settle_frame > current_frame && settle_frame - current_frame < fps * 3) {
                    for (int i = 0; i < settle_frame - current_frame - 1; ++i) cap.grab();
                    cap >> frame;
                    current_frame = settle_frame + 1;
                } else {
                    cap.set(cv::CAP_PROP_POS_FRAMES, settle_frame);
                    cap >> frame;
                    current_frame = settle_frame + 1;
                }

                if (!frame.empty()) {
                    cv::Mat settle_bgr = frame(cv::Rect(geo_.bx, geo_.by, geo_.bw, geo_.bh));
                    cv::Mat settle_gray;
                    cv::cvtColor(settle_bgr, settle_gray, cv::COLOR_BGR2GRAY);

                    cv::Mat settle_diff;
                    cv::absdiff(settle_gray, board_image_history.back(), settle_diff);
                    auto settle_best = score_moves_for_board(settle_diff);

                    if (settle_best.score > 25.0 && settle_best.from_sq >= 0) {
                        std::string settle_from = sq_name(settle_best.from_sq);
                        std::string settle_to = sq_name(settle_best.to_sq);
                        // Accept if same move or strictly better score
                        if (settle_best.score > best.score) {
                            t = settle_t;
                            board_gray = settle_gray;
                            diff = settle_diff;
                            best = settle_best;
                            from_name = settle_from;
                            to_name = settle_to;
                            move_uci = settle_from + settle_to;
                        }
                    }
                }

                // Restore stream position to just after the originally detected frame
                cap.set(cv::CAP_PROP_POS_FRAMES, target_frame);
                cap >> frame;
                current_frame = target_frame + 1;
            }

            // Inverse move filter: reject if this is the reverse of a recent move
            bool inverse_recent = false;
            std::string reverse_uci = to_name + from_name;
            size_t start = data.moves.size() > 4 ? data.moves.size() - 4 : 0;
            for (size_t i = start; i < data.moves.size(); ++i) {
                if (data.moves[i] == reverse_uci) { inverse_recent = true; break; }
            }
            if (inverse_recent && best.score < 70.0) {
                t = round_t(t + fine_step);
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
                t = round_t(t + fine_step);
                continue;
            }

            // ── Validation 1: Yellow square check ────────────────────────────
            // board_bgr is already cropped to the board region (same as Python)
            auto is_yellow = [board_bgr = board_bgr, geo = geo_](const std::string& sq_name) {
                int col = sq_name[0] - 'a';
                int row = 8 - (sq_name[1] - '0');
                int y1 = static_cast<int>(row * geo.sq_h);
                int y2 = static_cast<int>((row + 1) * geo.sq_h);
                int x1 = static_cast<int>(col * geo.sq_w);
                int x2 = static_cast<int>((col + 1) * geo.sq_w);
                int ch = static_cast<int>(geo.sq_h * 0.12);
                int cw = static_cast<int>(geo.sq_w * 0.12);

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
                    cv::Mat patch;
                    board_bgr(c).convertTo(patch, CV_32FC3);
                    std::vector<cv::Mat> ch;
                    cv::split(patch, ch);
                    cv::Scalar m = cv::mean((ch[2] + ch[1]) / 2.0f - ch[0]);
                    y_score += m[0];
                }
                return y_score / 4.0;
            };

            double y_from = is_yellow(from_name);
            double y_to = is_yellow(to_name);
            if (y_from < 40.0 || y_to < 40.0) {
                if (debug_level_ != DebugLevel::None) {
                    std::cout << "    [Debug] " << t << "s: " << move_uci << " rejected (Missing yellow highlights)\n";
                }
                t = round_t(t + fine_step);
                continue;
            }

            // ── Validation 2: Hover box rejection ────────────────────────────
            auto has_hover_box = [board_bgr = board_bgr, geo = geo_](const std::string& sq_name) {
                int col = sq_name[0] - 'a';
                int row = 8 - (sq_name[1] - '0');
                int y1 = static_cast<int>(row * geo.sq_h);
                int y2 = static_cast<int>((row + 1) * geo.sq_h);
                int x1 = static_cast<int>(col * geo.sq_w);
                int x2 = static_cast<int>((col + 1) * geo.sq_w);

                // Clamp to frame bounds
                int fh = board_bgr.rows, fw = board_bgr.cols;
                x1 = std::max(0, std::min(x1, fw - 1));
                y1 = std::max(0, std::min(y1, fh - 1));
                x2 = std::max(x1 + 1, std::min(x2, fw));
                y2 = std::max(y1 + 1, std::min(y2, fh));

                cv::Mat sq_bgr = board_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1));
                cv::Mat white_mask;
                cv::inRange(sq_bgr, cv::Scalar(160, 160, 160), cv::Scalar(255, 255, 255), white_mask);

                int thickness = std::max(3, static_cast<int>(geo.sq_w * 0.08));
                cv::Mat top = white_mask(cv::Rect(0, 0, x2 - x1, thickness));
                cv::Mat bottom = white_mask(cv::Rect(0, y2 - y1 - thickness, x2 - x1, thickness));
                cv::Mat left = white_mask(cv::Rect(0, 0, thickness, y2 - y1));
                cv::Mat right = white_mask(cv::Rect(x2 - x1 - thickness, 0, thickness, y2 - y1));

                int w = x2 - x1, h = y2 - y1;
                cv::Mat col_max_top, col_max_bot;
                cv::reduce(top, col_max_top, 0, cv::REDUCE_MAX);
                cv::reduce(bottom, col_max_bot, 0, cv::REDUCE_MAX);
                cv::Mat row_max_left, row_max_right;
                cv::reduce(left, row_max_left, 1, cv::REDUCE_MAX);
                cv::reduce(right, row_max_right, 1, cv::REDUCE_MAX);

                double ratios[4] = {
                    static_cast<double>(cv::countNonZero(col_max_top)) / std::max(1, w),
                    static_cast<double>(cv::countNonZero(col_max_bot)) / std::max(1, w),
                    static_cast<double>(cv::countNonZero(row_max_left)) / std::max(1, h),
                    static_cast<double>(cv::countNonZero(row_max_right)) / std::max(1, h)
                };
                int visible = 0;
                for (double r : ratios) { if (r > 0.10) ++visible; }
                return visible >= 2;
            };

            if (has_hover_box(to_name)) {
                if (debug_level_ != DebugLevel::None) {
                    std::cout << "    [Debug] " << t << "s: " << move_uci << " rejected (Piece is still mid-drag)\n";
                }
                t = round_t(t + fine_step);
                continue;
            }

            // ── Validation 3: Clock turn check ───────────────────────────────
            ClockState clocks = extract_clocks(frame, board_template_, geo_);
            if (!clocks.active_player.empty()) {
                std::string expected = (pos_ptr_->turn() == libchess::Side::White) ? "black" : "white";
                if (clocks.active_player != expected) {
                    if (debug_level_ != DebugLevel::None) {
                        std::cout << "    [Debug] " << t << "s: " << move_uci << " rejected (Waiting for clock to flip)\n";
                    }
                    t = round_t(t + fine_step);
                    continue;
                }
            }

            // ── All validations passed — accept the move ─────────────────────
            data.moves.push_back(move_uci);
            data.timestamps.push_back(t);

            std::cout << "[Branch " << branch_counter << "] Ply " << data.moves.size()
                      << ": detected " << move_uci << " at " << t << "s (confidence: " << round_t(score_to_confidence(best.score)) << "%)\n";

            // --- ADDED VISIBILITY: Print Top 3 Candidates ---
            if (debug_level_ != DebugLevel::None) {
                std::vector<double> sq_diffs(64, 0.0);
                for (const auto& sl : sq_slices_) {
                    cv::Mat region = diff(cv::Rect(sl.x1, sl.y1, sl.x2 - sl.x1, sl.y2 - sl.y1));
                    sq_diffs[parse_square_idx(sl.name)] = cv::mean(region)[0];
                }
                
                struct Cand { std::string uci; double score; };
                std::vector<Cand> cands;
                
                std::string fen = pos_ptr_->get_fen();
                
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
                            char p = get_piece_at(fen, f);
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
                    cands.push_back({sq_name(f) + sq_name(to), s});
                }
                std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b){ return a.score > b.score; });
                
                std::cout << "    > Top candidates: ";
                for (size_t i = 0; i < std::min<size_t>(3, cands.size()); ++i) {
                    std::cout << cands[i].uci << " (" << round_t(score_to_confidence(cands[i].score)) << "%)   ";
                }
                std::cout << "\n";
            }

            // Apply the move in libchess to update position state
            pos_ptr_->makemove(validated_move);

            // Update FEN, board image history, and clock history
            data.fens.push_back(pos_ptr_->get_fen());
            board_image_history.push_back(board_gray.clone());

            data.clocks.push_back({clocks.active_player, clocks.white_time, clocks.black_time});

            if (debug_level_ != DebugLevel::None) {
                char fname[80];
                snprintf(fname, sizeof(fname), "%s/%02d_b%d_%s_%.2fs.png",
                         debug_dir.c_str(), static_cast<int>(data.moves.size()),
                         branch_counter, move_uci.c_str(), t);
                cv::imwrite(fname, frame);
            }

            mode_fast = true;
            t = round_t(t + fast_step);
            continue;
        }

        t = round_t(t + fine_step);
    }

    cap.release();

    // Write JSON output
    std::cout << "Writing output to " << output_path << "\n";
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
