// Extracted from cpp directory
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"
#include "ChessFenUtils.h"
#include <sstream>
#include <iostream> // Required for std::cerr
#include <iomanip> // For std::fixed, std::setprecision
#include <array>
#include <cctype>
#include <cmath>

namespace cta {

namespace {
std::string normalize_clock(const std::string& raw) {
    if (raw.empty()) return "";

    // 1. Clean up duplicate colons and ensure only digits/colons remain
    std::string cleaned;
    bool last_was_colon = false;
    for (char c : raw) {
        if (std::isdigit(c) || c == '.') {
            cleaned += c;
            last_was_colon = false;
        } else if (c == ':') {
            if (!last_was_colon) {
                cleaned += c;
                last_was_colon = true;
            }
        }
    }

    // Trim leading/trailing colons
    if (!cleaned.empty() && cleaned.front() == ':') cleaned = cleaned.substr(1);
    if (!cleaned.empty() && cleaned.back() == ':') cleaned.pop_back();

    // 2. Split into components
    std::vector<std::string> parts;
    std::stringstream ss(cleaned);
    std::string item;
    while (std::getline(ss, item, ':')) {
        parts.push_back(item);
    }

    if (parts.size() > 3 || parts.empty()) return "";

    int h = 0, m = 0;
    double s = 0.0;
    try {
        if (parts.size() == 3) {
            h = std::stoi(parts[0]);
            m = std::stoi(parts[1]);
            s = std::stod(parts[2]);
        } else if (parts.size() == 2) {
            m = std::stoi(parts[0]);
            s = std::stod(parts[1]);
        } else {
            s = std::stod(parts[0]);
        }

        // 3. Strict Validation & Formatting
        if (s >= 60.0) return ""; // Discard OCR garbage like "020002"

        // Roll over excess minutes into hours (e.g., 90:00 -> 1:30:00)
        if (m > 59) {
            h += m / 60;
            m = m % 60;
        }
    } catch (...) {
        return ""; // Failsafe for std::stoi/stod exceptions
    }

    std::ostringstream out;
    out << h << ":" << std::setfill('0') << std::setw(2) << m << ":";
    
    // Seconds with proper zero-padding and precision handling
    if (s < 10.0) {
        out << "0";
    }
    
    if (std::floor(s) == s) {
        out << static_cast<int>(s);
    } else {
        // Show tenths of a second
        out << std::fixed << std::setprecision(1) << s;
    }
    
    return out.str();
}
} // namespace

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

        // Separate pure UCI from annotation (e.g. "e2e4!!" -> "e2e4" + "!!")
        std::string pure_uci;
        std::string annotation;
        size_t uci_len = 0;
        while (uci_len < uci_move_str.length()) {
            char c = uci_move_str[uci_len];
            if ((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8') || c == 'q' || c == 'r' || c == 'b' || c == 'n') {
                uci_len++;
            } else {
                break;
            }
        }
        pure_uci = uci_move_str.substr(0, uci_len);
        annotation = uci_move_str.substr(uci_len);

        try {
            move = current_pos.parse_move(pure_uci);
            san_move = ChessFenUtils::build_san(current_pos, move, pure_uci) + annotation;
            current_pos.makemove(move);
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to parse or convert move " << pure_uci << ": " << e.what() << std::endl;
            san_move = uci_move_str; // Fallback to UCI on error
        }

        // The PgnPly struct members are ordered {san, clock, evaluation_comment}.
        // The function parameters are (move_str, clock, eval_comment), so a direct mapping is correct.
        // The first parameter is now the converted SAN move.
        PgnPly ply{san_move, normalize_clock(clock), eval_comment, {}};
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
        ply.evaluation_comment = ChessFenUtils::format_eval_string(result.lines[0], result.fen);

        // Add all top N engine lines as variations
        for (const auto& line : result.lines) {
            if (line.move_uci == "ANNOTATION") continue; // Skip dummy video annotation lines

            std::vector<PgnPly> variation_line;
            libchess::Position var_pos(result.fen);
            std::istringstream pv_stream(line.pv_line);
            std::string move_uci_str;
            int move_count = 0;

            while (move_count < analysis_depth && (pv_stream >> move_uci_str)) {
                try {
                    libchess::Move m = var_pos.parse_move(move_uci_str);
                    std::string san = ChessFenUtils::build_san(var_pos, m, move_uci_str);
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
                variation_line[0].evaluation_comment = ChessFenUtils::format_eval_string(line, result.fen);
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

} // namespace cta