#include "PgnWriter.h"
#include <sstream>

namespace aa {

PgnWriter::PgnWriter() {
    active_lines_.push_back(&main_line_);
}

void PgnWriter::add_header(const std::string& key, const std::string& value) {
    headers_.push_back({key, value});
}

void PgnWriter::add_ply(const std::string& san, const std::string& clock) {
    if (active_lines_.empty()) return;
    PgnPly ply{san, clock, {}};
    active_lines_.back()->push_back(ply);
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