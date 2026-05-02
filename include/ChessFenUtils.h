#pragma once

#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include "StockfishAnalyzer.h" // For StockfishLine

#include <string>
#include <array>
#include <optional>

namespace cta {
namespace ChessFenUtils {

    std::array<char, 64> expand_fen_to_board(const std::string& fen);
    std::string build_san(const libchess::Position& pos, const libchess::Move& move, const std::string& uci_str);
    std::string uci_to_san_line(const std::string& uci_line, const std::string& start_fen);
    std::string format_eval_string(const StockfishLine& line, const std::string& fen);
    double get_line_score_cp(const StockfishLine& line);
    double score_from_analysis(const std::optional<StockfishResult>& analysis, const std::string& fen);

} // namespace ChessFenUtils
} // namespace cta
