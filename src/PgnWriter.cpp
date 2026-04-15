// Extracted from cpp directory
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"
#include <sstream>
#include <iostream> // Required for std::cerr
#include <iomanip> // For std::fixed, std::setprecision

namespace aa {

PgnWriter::PgnWriter() {
    active_lines_.push_back(&main_line_);
}

void PgnWriter::add_header(const std::string& key, const std::string& value) {
    headers_.push_back({key, value});
}

void PgnWriter::add_ply(const std::string& move_str, const std::string& clock, const std::string& eval_comment) {
    if (active_lines_.empty()) return;
    PgnPly ply{move_str, clock, eval_comment, {}};
    active_lines_.back()->push_back(ply);
}

void PgnWriter::add_variation_ply(libchess::Position& current_var_pos, const std::string& uci_move_str) {
    libchess::Move var_move;
    try {
        var_move = current_var_pos.parse_move(uci_move_str);
        active_lines_.back()->push_back({uci_move_str, "", "", {}});
        current_var_pos.makemove(var_move); // Update position for next move in PV
    } catch (const std::exception& e) {
        // Handle parsing error, e.g., log it or add UCI directly
        active_lines_.back()->push_back({uci_move_str, "", "", {}});
        std::cerr << "Warning: Failed to parse variation move " << uci_move_str << ": " << e.what() << std::endl;
    }
}

void PgnWriter::push_variation() {
    if (active_lines_.empty() || active_lines_.back()->empty()) return;

    auto* current_line = active_lines_.back();

    // A variation is an alternative to the most recently played move.
    // Therefore, we attach the variation list to the ply immediately *preceding* it.
    if (current_line->size() >= 2) {
        auto& prev_ply = (*current_line)[current_line->size() - 2];
        prev_ply.variations.push_back({});
        active_lines_.push_back(&prev_ply.variations.back());
    } else if (current_line->size() == 1) {
        // Edge case: variation on the very first move.
        // We append the variation to the first move itself to capture the branch.
        auto& first_ply = current_line->back();
        first_ply.variations.push_back({});
        active_lines_.push_back(&first_ply.variations.back());
    }
}

void PgnWriter::pop_variation() {
    if (active_lines_.size() > 1) {
        active_lines_.pop_back();
    }
}

void PgnWriter::add_stockfish_analysis(const std::vector<StockfishResult>& results) {
    // Stockfish results correspond to positions, not plies.
    // We need to add them as comments on the moves that follow those positions.
    // The FEN at index i corresponds to the position BEFORE move i is played.
    
    for (size_t i = 0; i < results.size() && i < main_line_.size(); ++i) {
        const auto& result = results[i];
        
        // Add evaluation as a comment on the move
        if (i < main_line_.size()) {
            auto& ply = main_line_[i];
            
            if (!result.lines.empty()) {
                const auto& best_line = result.lines[0];
                
                // Format: [%eval +0.42] or [%eval #-3]
                std::string eval_str;
                if (best_line.is_mate) {
                    eval_str = (best_line.mate_in > 0) ? 
                        "#+" + std::to_string(best_line.mate_in) : 
                        "#-" + std::to_string(std::abs(best_line.mate_in));
                } else { // Centipawns
                    double eval_cp = best_line.centipawns / 100.0;
                    eval_str = (eval_cp >= 0 ? "+" : "") + std::to_string(eval_cp);
                    // Remove trailing zeros
                    eval_str.erase(eval_str.find_last_not_of('0') + 1, std::string::npos);
                    if (eval_str.back() == '.') eval_str.pop_back();
                }
                
                ply.evaluation_comment = eval_str;
                
                // Add alternative lines as variations (skip the best line which is main)
                for (size_t j = 1; j < result.lines.size(); ++j) {
                    const auto& alt_line = result.lines[j];
                    
                    // Push variation before adding alternative moves
                    push_variation();
                    
                    // Parse PV line and add as variation moves
                    libchess::Position current_var_pos(result.fen); // Initialize with the FEN of the analyzed position
                    std::istringstream pv_stream(alt_line.pv_line);
                    std::string move_uci_str;
                    while (pv_stream >> move_uci_str) {
                        add_variation_ply(current_var_pos, move_uci_str);
                    }
                    
                    pop_variation();
                }
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
    build_line(oss, main_line_, 0, 0);
    oss << "\n*\n";

    return oss.str();
}

void PgnWriter::build_line(std::ostringstream& oss, const std::vector<PgnPly>& line, int starting_ply_count, int indent_level) const {
    std::string indent(indent_level * 4, ' ');
    int current_ply = starting_ply_count;

    for (size_t i = 0; i < line.size(); ++i) {
        const auto& ply = line[i];
        bool is_white = (current_ply % 2 == 0);
        int move_num = (current_ply / 2) + 1;

        if (is_white) {
            // Enforce exactly 1 move per line for main sequence
            if (i > 0 && indent_level == 0) oss << "\n";
            // If variation, stick to space delimitations
            else if (i > 0) oss << " ";

            oss << indent << move_num << ". " << ply.san;
        } else {
            if (i == 0) {
                // First ply in a sub-line is black's
                if (indent_level > 0) oss << indent;
                oss << move_num << "... " << ply.san;
            } else {
                oss << " " << ply.san;
            }
        }

        // Inject Evaluation Comments
        if (!ply.evaluation_comment.empty()) {
            oss << " {[%eval " << ply.evaluation_comment << "]}";
        }

        // Inject Clocks
        if (!ply.clock.empty()) {
            oss << " {[%clk " << ply.clock << "]}";
        }

        // Print nested variations
        for (const auto& var : ply.variations) {
            oss << "\n" << indent << "  (";
            build_line(oss, var, current_ply, indent_level + 1);
            oss << ")";

            // If the variation finishes and we are keeping this sequence,
            // we must cleanly print the move number again so contexts aren't lost.
            if (i + 1 < line.size()) {
                if (indent_level == 0) oss << "\n" << indent;
                else oss << " ";

                if ((current_ply + 1) % 2 != 0) { // If the next move is Black's
                    oss << move_num << "...";
                }
            }
        }

        current_ply++;
    }
}

} // namespace aa