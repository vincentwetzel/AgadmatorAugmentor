#pragma once

#include "libchess/position.hpp"
#include "StockfishAnalyzer.h"
#include <string>
#include <vector>
#include <list>

namespace cta {

struct PgnPly {
    std::string san;
    std::string clock;
    std::string evaluation_comment;
    std::vector<std::vector<PgnPly>> variations;
};

class PgnWriter {
public:
    PgnWriter();

    void add_header(const std::string& key, const std::string& value);
    void add_ply(const std::string& uci_move_str, const std::string& clock = "", const std::string& eval_comment = "");
    void add_stockfish_analysis(const std::vector<StockfishResult>& results, int analysis_depth);
    void push_variation();
    void pop_variation();

    std::string build() const;

private:

    void build_line(std::ostringstream& oss, const std::vector<PgnPly>& line, int starting_ply_count, int indent_level) const;

    std::vector<std::pair<std::string, std::string>> headers_;
    std::vector<PgnPly> main_line_;

    // State for building variations
    std::vector<std::vector<PgnPly>*> active_lines_;

    // State for SAN conversion and position tracking
    libchess::Position pos_;
    std::vector<libchess::Position> pos_stack_;
};

} // namespace cta