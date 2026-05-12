#include "ChessVideoExtractor_Internal.h"

#include "ChessFenUtils.h"
#include "ExtractorUtils.h"
#include "MoveValidations.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string_view>

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
    cv::VideoCapture cap(safe_video_path, cv::CAP_FFMPEG, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY, cv::CAP_PROP_HW_DEVICE, 0});
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_FFMPEG, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
    if (!cap.isOpened()) cap.open(safe_video_path, cv::CAP_ANY, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
    return cap;
}

bool is_inverse_of_recent_move(const std::vector<std::string>& moves, const char* from_name, const char* to_name) {
    char reverse_uci_buf[5] = {to_name[0], to_name[1], from_name[0], from_name[1], '\0'};
    std::string_view reverse_uci(reverse_uci_buf, 4);
    size_t start = moves.size() > 4 ? moves.size() - 4 : 0;
    for (size_t i = start; i < moves.size(); ++i) {
        if (moves[i] == reverse_uci) return true;
    }
    return false;
}

bool passes_yellowness_check(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* from_name, const char* to_name) {
    double y_from = validation::check_yellowness(board_bgr, geo, from_name);
    double y_to = validation::check_yellowness(board_bgr, geo, to_name);
    return !(y_from < 25.0 || y_to < 25.0 || (y_from + y_to) < 70.0);
}

bool is_valid_libchess_move(libchess::Position& pos, const std::string& move_uci, libchess::Move& out_move) {
    try {
        out_move = pos.parse_move(move_uci);
        return true;
    } catch (...) {
        return false;
    }
}

void adjust_rook_target(int& to_sq, const char*& to_name, int from_sq, const char* from_name, const cv::Mat& board_bgr, const BoardGeometry& geo, libchess::Position* pos_ptr) {
    int from_file = from_sq & 7, from_rank = from_sq >> 3, to_file = to_sq & 7, to_rank = to_sq >> 3, alt_to = -1;
    if (from_rank == to_rank && std::abs(to_file - from_file) > 1) alt_to = from_sq + (to_file > from_file ? 1 : -1);
    else if (from_file == to_file && std::abs(to_rank - from_rank) > 1) alt_to = from_sq + (to_rank > from_rank ? 8 : -8);
    if (alt_to >= 0) { const char* alt_to_name = utils::sq_name(alt_to); char alt_uci[5] = {from_name[0], from_name[1], alt_to_name[0], alt_to_name[1], '\0'}; try { (void)pos_ptr->parse_move(alt_uci); double y_alt = validation::check_yellowness(board_bgr, geo, alt_to_name), y_best = validation::check_yellowness(board_bgr, geo, to_name); if (y_alt >= 25.0 && y_alt > y_best + 10.0) { to_sq = alt_to; to_name = alt_to_name; } } catch (...) {} }
    if (from_sq == 5 && to_sq == 3) { try { (void)pos_ptr->parse_move("f1e1"); to_sq = 4; to_name = utils::sq_name(to_sq); } catch (...) {} }
}

} // namespace cta::extractor_detail
