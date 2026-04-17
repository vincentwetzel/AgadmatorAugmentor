// Extracted from cpp directory
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"
#include <sstream>
#include <iostream> // Required for std::cerr
#include <iomanip> // For std::fixed, std::setprecision
#include <array>
#include <cctype>
#include <cmath>

namespace aa {

namespace {
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

    std::string format_eval_string(const StockfishLine& line, const std::string& fen) {
        bool is_black_to_move = (fen.find(" b ") != std::string::npos);

        if (line.is_mate) {
            int mate_in = line.mate_in;
            if (is_black_to_move) mate_in = -mate_in;
            if (mate_in > 0) return "+M" + std::to_string(mate_in);
            else return "-M" + std::to_string(-mate_in);
        }

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        double eval_cp = line.centipawns / 100.0;
        if (is_black_to_move) eval_cp = -eval_cp;
        
        if (eval_cp >= 0.0) ss << "+";
        ss << eval_cp;
        return ss.str();
    }

    std::string build_san(const libchess::Position& pos, const libchess::Move& move, const std::string& uci_str) {
        auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
        auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));
        
        std::array<char, 64> board = expand_fen_to_board(pos.get_fen());
        char piece = board[from_sq];
        char target_piece = board[to_sq];
        
        bool is_pawn = (piece == 'P' || piece == 'p');
        bool is_capture = (target_piece != ' ') || (is_pawn && (from_sq % 8) != (to_sq % 8) && target_piece == ' ');
        
        // Castling
        if (move.type() == libchess::MoveType::ksc) return "O-O";
        if (move.type() == libchess::MoveType::qsc) return "O-O-O";

        if ((piece == 'K' || piece == 'k') && std::abs((from_sq % 8) - (to_sq % 8)) == 2) {
            if (to_sq % 8 == 6) return "O-O";
            if (to_sq % 8 == 2) return "O-O-O";
        }

        std::string san;
        if (!is_pawn) {
            san += static_cast<char>(std::toupper(piece));
            
            // Disambiguation
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
                if (!file_conflict) {
                    san += static_cast<char>('a' + (from_sq % 8));
                } else if (!rank_conflict) {
                    san += static_cast<char>('1' + (from_sq / 8));
                } else {
                    san += static_cast<char>('a' + (from_sq % 8));
                    san += static_cast<char>('1' + (from_sq / 8));
                }
            }
        } else {
            if (is_capture) {
                san += static_cast<char>('a' + (from_sq % 8));
            }
        }
        
        if (is_capture) san += "x";
        
        san += static_cast<char>('a' + (to_sq % 8));
        san += static_cast<char>('1' + (to_sq / 8));
        
        // Promotion
        if (uci_str.length() >= 5) {
            san += "=";
            san += static_cast<char>(std::toupper(uci_str[4]));
        }
        
        // Append check/checkmate symbol, which is standard for PGN.
        // A temporary position is used to check the state *after* the move.
        libchess::Position temp_pos = pos;
        temp_pos.makemove(move);
        if (temp_pos.is_checkmate()) {
            san += "#";
        } else if (temp_pos.in_check()) {
            san += "+";
        }

        return san;
    }
}

PgnWriter::PgnWriter() {
    pos_.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    active_lines_.push_back(&main_line_);
}

void PgnWriter::add_header(const std::string& key, const std::string& value) {
    headers_.push_back({key, value});
}
void PgnWriter::add_ply(const std::string& uci_move_str, const std::string& clock, const std::string& eval_comment) {
    if (active_lines_.empty()) return;

    libchess::Position& current_pos = active_lines_.size() > 1 ? pos_stack_.back() : pos_;
    libchess::Move move;
    std::string san_move;

    try {
        move = current_pos.parse_move(uci_move_str);
        san_move = build_san(current_pos, move, uci_move_str);
        current_pos.makemove(move);
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to parse or convert move " << uci_move_str << ": " << e.what() << std::endl;
        san_move = uci_move_str; // Fallback to UCI on error
    }

    // The PgnPly struct members are ordered {san, clock, evaluation_comment}.
    // The function parameters are (move_str, clock, eval_comment), so a direct mapping is correct.
    // The first parameter is now the converted SAN move.
    PgnPly ply{san_move, clock, eval_comment, {}};
    active_lines_.back()->push_back(ply);
}

void PgnWriter::push_variation() {
    if (active_lines_.empty() || active_lines_.back()->empty()) return;
    auto* current_line = active_lines_.back();
    auto& last_ply = current_line->back();

    // Save current position state before branching
    libchess::Position& pos_to_branch_from = active_lines_.size() > 1 ? pos_stack_.back() : pos_;
    libchess::Position new_var_pos = pos_to_branch_from;
    new_var_pos.undomove(); // Undo the last move to start the variation from the same board state
    pos_stack_.push_back(new_var_pos);

    // Create and switch to the new variation line
    last_ply.variations.push_back({});
    active_lines_.push_back(&last_ply.variations.back());
}

void PgnWriter::pop_variation() {
    if (active_lines_.size() > 1) {
        // Restore position from before the variation
        pos_stack_.pop_back();
        active_lines_.pop_back();
    }
}

void PgnWriter::add_stockfish_analysis(const std::vector<StockfishResult>& results, int analysis_depth) {
    // Stockfish results correspond to positions.
    // The FEN at index i in the input `fens` vector corresponds to the position BEFORE move i is played.
    // The analysis for the position AFTER move `i` is therefore at `results[i+1]`.

    for (size_t i = 0; i < main_line_.size(); ++i) {
        if (i + 1 >= results.size()) continue;

        const auto& result = results[i + 1]; // Analysis of position after move `i`
        auto& ply = main_line_[i];

        if (result.lines.empty()) continue;

        // Add evaluation comment for the position on the board (after the played move)
        ply.evaluation_comment = format_eval_string(result.lines[0], result.fen);

        // Add all top N engine lines as variations
        for (const auto& line : result.lines) {
            std::vector<PgnPly> variation_line;
            libchess::Position var_pos(result.fen);
            std::istringstream pv_stream(line.pv_line);
            std::string move_uci_str;
            int move_count = 0;

            while (move_count < analysis_depth && (pv_stream >> move_uci_str)) {
                try {
                    libchess::Move m = var_pos.parse_move(move_uci_str);
                    std::string san = build_san(var_pos, m, move_uci_str);
                    variation_line.push_back({san, "", "", {}});
                    var_pos.makemove(m);
                } catch (...) {
                    // Fallback for parsing errors
                    variation_line.push_back({move_uci_str, "", "", {}});
                }
                move_count++;
            }

            if (!variation_line.empty()) {
                // Add the evaluation comment to the first move of the variation
                variation_line[0].evaluation_comment = format_eval_string(line, result.fen);
                ply.variations.push_back(std::move(variation_line));
            }
        }
    }
}


std::string PgnWriter::build() const {
    std::ostringstream oss;

    // Write Headers
    for (const auto& [k, v] : headers_) {
        oss << "[" << k << " \"" << v << "\"]\n";
    }
    if (!headers_.empty()) oss << "\n";

    // Build Moves Recursively
    build_line(oss, main_line_, 1, 0);
    oss << "\n*\n";

    return oss.str();
}

void PgnWriter::build_line(std::ostringstream& oss, const std::vector<PgnPly>& line, int starting_ply_count, int indent_level) const {
    std::string indent(indent_level * 4, ' ');
    int ply_number = starting_ply_count;

    for (size_t i = 0; i < line.size(); ++i) {
        const auto& ply = line[i];
        bool is_white = ((ply_number - 1) % 2 == 0);
        int move_num = (ply_number + 1) / 2;

        if (is_white) {
            if (indent_level == 0) {
                if (i > 0) oss << "\n";
                oss << indent << move_num << ". " << ply.san;
            } else {
                if (i > 0) oss << " ";
                oss << move_num << ". " << ply.san;
            }
        } else {
            if (i == 0) {
                oss << move_num << "... " << ply.san;
            } else {
                oss << " " << ply.san;
            }
        }

        // Inject Evaluation Comments
        if (!ply.evaluation_comment.empty()) {
            oss << " {Stockfish [%eval " << ply.evaluation_comment << "]}";
        }

        // Inject Clocks
        if (!ply.clock.empty()) {
            oss << " {[%clk " << ply.clock << "]}";
        }

        // Print nested variations
        for (size_t v = 0; v < ply.variations.size(); ++v) {
            const auto& var = ply.variations[v];
            oss << "\n" << indent << "  (";
            // A variation is an alternative for the CURRENT move, so ply_number remains the same
            build_line(oss, var, ply_number, indent_level + 1);
            oss << ")";

            // If all variations finished and we are keeping this sequence,
            // cleanly print the move number again so contexts aren't lost.
            if (v + 1 == ply.variations.size() && i + 1 < line.size()) {
                if (ply_number % 2 != 0) { // If current move is White, next is Black
                    if (indent_level == 0) oss << "\n" << indent;
                    else oss << " ";
                    
                    oss << move_num << "...";
                }
            }
        }
        ply_number++;
    }
}

} // namespace aa