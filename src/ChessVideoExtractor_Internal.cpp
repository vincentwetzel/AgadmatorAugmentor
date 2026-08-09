#include "ChessVideoExtractor_Internal.h"

#include "ChessFenUtils.h"
#include "ExtractorUtils.h"
#include "MoveValidations.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace cta::extractor_detail {

double score_to_confidence(double s) {
    if (s >= 60.0) return 99.9;
    if (s <= 0.0) return 0.0;
    if (s >= 25.0) return 50.0 + ((s - 25.0) / 35.0) * 49.9;
    return (s / 25.0) * 50.0;
}

double round_t(double val) {
    return std::round(val * 100.0) / 100.0;
}

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
            (void)temp_pos.makemove(m);
            std::array<char, 64> board_after = utils::expand_fen(temp_pos.get_fen());
            uci += static_cast<char>(std::tolower(board_after[to]));
        }
        cands.push_back({std::move(uci), s});
    }

    size_t k = std::min<size_t>(3, cands.size());
    std::partial_sort(cands.begin(), cands.begin() + k, cands.end(), [](const Cand& a, const Cand& b) {
        return a.score > b.score;
    });

    std::ostringstream cands_ss;
    cands_ss << "    " << utils::ts(elapsed) << " > Top candidates: ";
    for (size_t i = 0; i < k; ++i) {
        cands_ss << cands[i].uci << " (" << round_t(score_to_confidence(cands[i].score)) << "%)   ";
    }
    log_info(cands_ss.str());
}

cv::VideoCapture open_video_capture(const std::string& safe_video_path) {
    cv::VideoCapture cap(safe_video_path, cv::CAP_FFMPEG, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_ANY, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_FFMPEG);
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_ANY);
    return cap;
}

std::optional<size_t> find_recent_inverse_move_index(const std::vector<std::string>& moves,
                                                      const char* from_name,
                                                      const char* to_name) {
    char reverse_uci_buf[5] = {to_name[0], to_name[1], from_name[0], from_name[1], '\0'};
    std::string_view reverse_uci(reverse_uci_buf, 4);
    size_t start = moves.size() > 4 ? moves.size() - 4 : 0;
    for (size_t i = moves.size(); i-- > start;) {
        if (moves[i] == reverse_uci) return i;
    }
    return std::nullopt;
}

bool is_inverse_of_recent_move(const std::vector<std::string>& moves, const char* from_name, const char* to_name) {
    return find_recent_inverse_move_index(moves, from_name, to_name).has_value();
}

bool passes_yellowness_check(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* from_name, const char* to_name) {
    double y_from = validation::check_yellowness(board_bgr, geo, from_name);
    double y_to = validation::check_yellowness(board_bgr, geo, to_name);
    return !(y_from < validation::kYellowEndpointThreshold ||
             y_to < validation::kYellowEndpointThreshold ||
             (y_from + y_to) < validation::kYellowPairThreshold);
}

double square_piece_edge_score(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* sq_name) {
    if (board_bgr.empty() || sq_name == nullptr) return 0.0;

    const int col = sq_name[0] - 'a';
    const int rank = sq_name[1] - '1';
    if (col < 0 || col >= 8 || rank < 0 || rank >= 8) return 0.0;

    const int row = 7 - rank;
    const int x1 = std::max(0, static_cast<int>(col * geo.sq_w + geo.sq_w * 0.15));
    const int x2 = std::min(board_bgr.cols, static_cast<int>((col + 1) * geo.sq_w - geo.sq_w * 0.15));
    const int y1 = std::max(0, static_cast<int>(row * geo.sq_h + geo.sq_h * 0.15));
    const int y2 = std::min(board_bgr.rows, static_cast<int>((row + 1) * geo.sq_h - geo.sq_h * 0.15));
    if (x2 <= x1 || y2 <= y1) return 0.0;

    cv::Mat gray;
    cv::cvtColor(board_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1)), gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(3, 3), 0);
    cv::Mat edges;
    cv::Canny(blurred, edges, 40, 100);
    return cv::mean(edges)[0];
}

bool is_valid_libchess_move(libchess::Position& pos, const std::string& move_uci, libchess::Move& out_move) {
    try {
        out_move = pos.parse_move(move_uci);
        return true;
    } catch (...) {
        return false;
    }
}

void adjust_sliding_target(int& to_sq, const char*& to_name, int from_sq, const char* from_name, const std::vector<double>& sq_diffs, const cv::Mat& board_bgr, const BoardGeometry& geo, libchess::Position* pos_ptr) {
    const char* internal_trace_path = std::getenv("CTA_INTERNAL_TRACE_FILE");
    auto trace_endpoint_decision = [&](const char* stage, int candidate_sq, double current_y,
                                       double current_evidence, double candidate_y,
                                       double candidate_evidence, bool strong_diff) {
        if (internal_trace_path == nullptr || *internal_trace_path == '\0') return;
        std::ofstream trace(internal_trace_path, std::ios::out | std::ios::app);
        if (!trace.is_open()) return;
        trace << stage << '\t'
              << from_name << '\t' << utils::sq_name(to_sq) << '\t'
              << utils::sq_name(candidate_sq) << '\t'
              << current_y << '\t' << sq_diffs[to_sq] << '\t'
              << current_evidence << '\t' << candidate_y << '\t'
              << sq_diffs[candidate_sq] << '\t' << candidate_evidence << '\t'
              << (strong_diff ? 1 : 0) << '\n';
    };
    int from_file = from_sq & 7, from_rank = from_sq >> 3, to_file = to_sq & 7, to_rank = to_sq >> 3, alt_to = -1;
    if (from_rank == to_rank && std::abs(to_file - from_file) > 1) alt_to = from_sq + (to_file > from_file ? 1 : -1);
    else if (from_file == to_file && std::abs(to_rank - from_rank) > 1) alt_to = from_sq + (to_rank > from_rank ? 8 : -8);

    std::array<char, 64> board_map = utils::expand_fen(pos_ptr->get_fen());
    const char moving_piece = board_map[from_sq];
    auto is_occupied = [](char piece) {
        return piece != ' ' && piece != '.';
    };

    if (alt_to < 0 && (from_rank == to_rank || from_file == to_file)) {
        int step = 0;
        if (from_rank == to_rank && std::abs(to_file - from_file) == 1) {
            step = to_file > from_file ? 1 : -1;
        } else if (from_file == to_file && std::abs(to_rank - from_rank) == 1) {
            step = to_rank > from_rank ? 8 : -8;
        }

        if (step != 0) {
            const double current_y = validation::check_yellowness(board_bgr, geo, to_name);
            const double current_evidence = current_y + sq_diffs[to_sq];
            int best_far_sq = -1;
            double best_far_y = current_y;
            double best_far_evidence = 0.0;
            int nearest_candidate_sq = -1;
            double nearest_candidate_y = 0.0;
            double nearest_candidate_diff = 0.0;
            struct FlatSlidingSquare {
                int sq;
                double y;
            };
            std::vector<FlatSlidingSquare> edge_rank_run;
            const bool edge_rank_rescue = from_rank == to_rank && (from_file == 0 || from_file == 7);

            for (int sq = to_sq + step; sq >= 0 && sq < 64; sq += step) {
                if (step == 1 && (sq & 7) == 0) break;
                if (step == -1 && (sq & 7) == 7) break;

                const char* candidate_name = utils::sq_name(sq);
                char candidate_uci[5] = {from_name[0], from_name[1], candidate_name[0], candidate_name[1], '\0'};
                try {
                    (void)pos_ptr->parse_move(candidate_uci);
                } catch (...) {
                    break;
                }

                const char target_piece = board_map[sq];
                const bool captures_enemy = is_occupied(target_piece) &&
                    (std::isupper(static_cast<unsigned char>(moving_piece)) !=
                     std::isupper(static_cast<unsigned char>(target_piece)));
                if (captures_enemy) {
                    best_far_sq = sq;
                    break;
                }

                const double y = validation::check_yellowness(board_bgr, geo, candidate_name);
                trace_endpoint_decision("FAR_SCAN", sq, current_y, current_evidence,
                                        y, y + sq_diffs[sq], false);
                const int distance_steps = std::abs((sq - from_sq) / step);
                if (distance_steps == 2 && y >= 35.0 && y > best_far_y + 15.0) {
                    best_far_y = y;
                    best_far_evidence = y + sq_diffs[sq];
                    best_far_sq = sq;
                    nearest_candidate_sq = sq;
                    nearest_candidate_y = y;
                    nearest_candidate_diff = sq_diffs[sq];
                    edge_rank_run.clear();
                } else {
                    // The first legal square beyond the provisional target
                    // is two steps from the origin.  It is a common landing
                    // distance for sliders and must be scored before any
                    // farther square can win through highlight spillover.
                    if (distance_steps == 2 && (y >= 12.0 || sq_diffs[sq] >= 12.0)) {
                        const double evidence = y + sq_diffs[sq];
                        if (evidence > best_far_evidence) {
                            best_far_evidence = evidence;
                            best_far_y = y;
                            best_far_sq = sq;
                        }
                        nearest_candidate_sq = sq;
                        nearest_candidate_y = y;
                        nearest_candidate_diff = sq_diffs[sq];
                        if (edge_rank_rescue && sq_diffs[sq] <= 5.0 && y >= 25.0) {
                            edge_rank_run.push_back({sq, y});
                        } else {
                            edge_rank_run.clear();
                        }
                    } else if (distance_steps > 2 && nearest_candidate_sq >= 0 &&
                               sq_diffs[sq] + 5.0 < nearest_candidate_diff &&
                               y >= 30.0 && y + 12.0 >= nearest_candidate_y) {
                        // A nearer square with a large board difference can
                        // be an animated transit square.  Continue to a
                        // farther, cleaner yellow landing only when its
                        // difference drops materially and its registration
                        // remains comparable.  This handles long sliders
                        // without promoting arbitrary distant highlights.
                        best_far_y = y;
                        best_far_evidence = y + sq_diffs[sq];
                        best_far_sq = sq;
                    } else if (edge_rank_rescue && distance_steps == 2 &&
                               sq_diffs[sq] <= 5.0 && y >= 25.0) {
                        edge_rank_run.push_back({sq, y});
                    } else {
                        edge_rank_run.clear();
                    }
                }

                if (is_occupied(target_piece)) break;
            }

            if (edge_rank_run.size() >= 2) {
                auto [min_it, max_it] = std::minmax_element(
                    edge_rank_run.begin(), edge_rank_run.end(),
                    [](const FlatSlidingSquare& a, const FlatSlidingSquare& b) {
                        return a.y < b.y;
                    });
                if (max_it->y - min_it->y <= 8.0) {
                    best_far_sq = edge_rank_run[edge_rank_run.size() / 2].sq;
                    best_far_evidence = 22.0;
                }
            }

            // A short, already-highlighted landing is the strongest visual
            // registration we have.  Only promote it to a farther endpoint
            // when the farther square explains materially more evidence;
            // absolute evidence alone is vulnerable to animation shadows on
            // neighboring slider squares.
            constexpr double kMinimumFarEndpointGain = 8.0;
            constexpr double kMinimumFarYellowGain = 5.0;
            const bool far_endpoint_has_stronger_yellow_registration =
                best_far_sq >= 0 && best_far_y >= 35.0 &&
                best_far_y >= current_y + kMinimumFarYellowGain;
            if (best_far_sq >= 0 &&
                (far_endpoint_has_stronger_yellow_registration ||
                 best_far_evidence <= 0.0 ||
                  best_far_evidence >= current_evidence + kMinimumFarEndpointGain)) {
                trace_endpoint_decision("FAR_ACCEPT", best_far_sq, current_y, current_evidence,
                                        validation::check_yellowness(board_bgr, geo, utils::sq_name(best_far_sq)),
                                        best_far_evidence, false);
                to_sq = best_far_sq;
                to_name = utils::sq_name(best_far_sq);
                return;
            }
        }
    }

    if (alt_to >= 0) {
        int step = 0;
        if (from_rank == to_rank) {
            step = to_file > from_file ? 1 : -1;
        } else if (from_file == to_file) {
            step = to_rank > from_rank ? 8 : -8;
        }

        if (step != 0) {
            const double current_y = validation::check_yellowness(board_bgr, geo, to_name);
            const double current_evidence = current_y + sq_diffs[to_sq];
            // Once the registered destination itself contains a strong board
            // change, a neighboring square is almost certainly animation
            // spillover rather than the actual landing square.
            const bool current_target_has_strong_diff = sq_diffs[to_sq] >= 25.0;
            const int neighbors[2] = {to_sq - step, to_sq + step};
            int best_neighbor = -1;
            double best_neighbor_evidence = current_evidence;
            for (int candidate_sq : neighbors) {
                if (candidate_sq < 0 || candidate_sq >= 64 || candidate_sq == from_sq) continue;
                if (from_rank == to_rank && ((candidate_sq >> 3) != from_rank)) continue;
                if (from_file == to_file && ((candidate_sq & 7) != from_file)) continue;

                const char* candidate_name = utils::sq_name(candidate_sq);
                char candidate_uci[5] = {from_name[0], from_name[1], candidate_name[0], candidate_name[1], '\0'};
                try {
                    (void)pos_ptr->parse_move(candidate_uci);
                } catch (...) {
                    continue;
                }

                const double y = validation::check_yellowness(board_bgr, geo, candidate_name);
                const double evidence = y + sq_diffs[candidate_sq];
                const bool one_step_before_endpoint = candidate_sq == to_sq - step;
                const bool credible_short_landing =
                    one_step_before_endpoint && y >= 18.0 && evidence >= current_evidence + 4.0;
                if (!current_target_has_strong_diff &&
                    ((y >= 25.0 && evidence > best_neighbor_evidence + 10.0) || credible_short_landing)) {
                    trace_endpoint_decision("NEIGHBOR_ACCEPT", candidate_sq, current_y, current_evidence,
                                            y, evidence, current_target_has_strong_diff);
                    best_neighbor = candidate_sq;
                    best_neighbor_evidence = evidence;
                }
            }

            if (best_neighbor >= 0) {
                to_sq = best_neighbor;
                to_name = utils::sq_name(best_neighbor);
                return;
            }
        }
    }

    if (alt_to < 0) return;

    const char target_piece = board_map[to_sq];
    if (is_occupied(target_piece)) {
        const bool same_side = std::isupper(static_cast<unsigned char>(moving_piece)) ==
                               std::isupper(static_cast<unsigned char>(target_piece));
        if (!same_side) return;
    }

    const char* alt_to_name = utils::sq_name(alt_to);
    char alt_uci[5] = {from_name[0], from_name[1], alt_to_name[0], alt_to_name[1], '\0'};
    const bool current_target_has_strong_diff = sq_diffs[to_sq] >= 25.0;
    try {
        (void)pos_ptr->parse_move(alt_uci);
        double y_alt = validation::check_yellowness(board_bgr, geo, alt_to_name);
        double y_best = validation::check_yellowness(board_bgr, geo, to_name);
        const int distance_steps = std::max(std::abs(to_file - from_file), std::abs(to_rank - from_rank));
        const bool short_ambiguity = distance_steps <= 2;
        if (short_ambiguity && !current_target_has_strong_diff &&
            y_alt >= 25.0 && y_alt > y_best + 10.0) {
            trace_endpoint_decision("ALT_ACCEPT", alt_to, y_best, y_best + sq_diffs[to_sq],
                                    y_alt, y_alt + sq_diffs[alt_to], false);
            to_sq = alt_to;
            to_name = alt_to_name;
        }
    } catch (...) {
    }
}

} // namespace cta::extractor_detail
