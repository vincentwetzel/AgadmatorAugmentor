#include "ChessFenUtils.h"
#include <cctype>
#include <iomanip>
#include <sstream>
#include <algorithm> // For std::clamp

namespace cta {
namespace ChessFenUtils {

    std::array<char, 64> expand_fen_to_board(const std::string& fen) {
        std::array<char, 64> board;
        board.fill(' ');
        int sq = 56;
        for (char c : fen) {
            if (c == ' ') break;
            if (c == '/') sq -= 16;
            else if (c >= '1' && c <= '8') sq += (c - '0');
            else board[sq++] = c;
        }
        return board;
    }

    std::string build_san(const libchess::Position& pos, const libchess::Move& move, const std::string& uci_str) {
        auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
        auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));
        
        std::array<char, 64> board = expand_fen_to_board(pos.get_fen());
        char piece = board[from_sq];
        char target_piece = board[to_sq];
        
        bool is_pawn = (piece == 'P' || piece == 'p');
        bool is_capture = (target_piece != ' ') || (is_pawn && (from_sq % 8) != (to_sq % 8) && target_piece == ' ');
        
        if (move.type() == libchess::MoveType::ksc) return "O-O";
        if (move.type() == libchess::MoveType::qsc) return "O-O-O";

        if ((piece == 'K' || piece == 'k') && std::abs((from_sq % 8) - (to_sq % 8)) == 2) {
            if (to_sq % 8 == 6) return "O-O";
            if (to_sq % 8 == 2) return "O-O-O";
        }

        std::string san;
        if (!is_pawn) {
            san += static_cast<char>(std::toupper(piece));
            bool file_conflict = false;
            bool rank_conflict = false;
            bool need_disambiguation = false;
            
            for (const auto& alt_move : pos.legal_moves()) {
                auto alt_from = static_cast<int>(static_cast<unsigned int>(alt_move.from()));
                auto alt_to = static_cast<int>(static_cast<unsigned int>(alt_move.to()));
                
                if (alt_from != from_sq && alt_to == to_sq && board[alt_from] == piece) {
                    need_disambiguation = true;
                    if (alt_from % 8 == from_sq % 8) file_conflict = true;
                    if (alt_from / 8 == from_sq / 8) rank_conflict = true;
                }
            }
            
            if (need_disambiguation) {
                if (!file_conflict) san += static_cast<char>('a' + (from_sq % 8));
                else if (!rank_conflict) san += static_cast<char>('1' + (from_sq / 8));
                else {
                    san += static_cast<char>('a' + (from_sq % 8));
                    san += static_cast<char>('1' + (from_sq / 8));
                }
            }
        } else {
            if (is_capture) san += static_cast<char>('a' + (from_sq % 8));
        }
        
        if (is_capture) san += "x";
        san += static_cast<char>('a' + (to_sq % 8));
        san += static_cast<char>('1' + (to_sq / 8));
        
        if (uci_str.length() >= 5) {
            san += "=";
            san += static_cast<char>(std::toupper(uci_str[4]));
        }
        
        libchess::Position temp_pos = pos;
        temp_pos.makemove(move);
        if (temp_pos.is_checkmate()) san += "#";
        else if (temp_pos.in_check()) san += "+";

        return san;
    }

    std::string uci_to_san_line(const std::string& uci_line, const std::string& start_fen) {
        std::istringstream iss(uci_line);
        std::string uci_move;
        std::string san_line;
        try {
            libchess::Position pos(start_fen);
            while (iss >> uci_move) {
                if (!san_line.empty()) san_line += " ";
                libchess::Move m = pos.parse_move(uci_move);
                san_line += build_san(pos, m, uci_move);
                pos.makemove(m);
            }
        } catch (...) {
            return uci_line; // fallback to original string on parsing error
        }
        return san_line.empty() ? uci_line : san_line;
    }

    std::string format_eval_string(const StockfishLine& line, const std::string& fen) {
        bool is_black_to_move = (fen.find(" b ") != std::string::npos);

        if (line.is_mate) {
            int mate_in = line.mate_in;
            if (is_black_to_move) mate_in = -mate_in;
            
            if (mate_in > 0) {
                return "+M" + std::to_string(mate_in);
            } else {
                return "-M" + std::to_string(-mate_in);
            }
        }

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        double eval_cp = line.centipawns / 100.0;
        if (is_black_to_move) eval_cp = -eval_cp;
        
        if (eval_cp >= 0.0) {
            ss << "+";
        }
        ss << eval_cp;
        return ss.str();
    }

    double get_line_score_cp(const StockfishLine& line) {
        if (line.is_mate) {
            return (line.mate_in > 0) ? (10000.0 - line.mate_in) : (-10000.0 - line.mate_in);
        }
        return static_cast<double>(line.centipawns);
    }

    double score_from_analysis(const std::optional<StockfishResult>& analysis, const std::string& fen) {
        if (!analysis.has_value() || analysis->lines.empty()) {
            return 0.0;
        }

        const auto& best_line = analysis->lines[0];
        double score = 0.0;
        if (best_line.is_mate) {
            score = (best_line.mate_in > 0) ? 15000.0 : -15000.0;
        } else {
            score = static_cast<double>(best_line.centipawns);
        }
        
        if (fen.find(" b ") != std::string::npos) {
            score = -score;
        }

        return score;
    }

} // namespace ChessFenUtils
} // namespace cta
