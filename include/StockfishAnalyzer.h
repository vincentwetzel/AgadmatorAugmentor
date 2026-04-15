#pragma once

#include <string>
#include <vector>
#include <memory>

namespace aa {

struct StockfishLine {
    std::string move_uci;
    std::string pv_line;
    int centipawns = 0;
    int mate_in = 0;
    bool is_mate = false;
};

struct StockfishResult {
    std::string fen;
    std::vector<StockfishLine> lines;
};

class StockfishAnalyzer {
public:
    explicit StockfishAnalyzer(int multi_pv = 1);
    ~StockfishAnalyzer();

    // Delete copy/move
    StockfishAnalyzer(const StockfishAnalyzer&) = delete;
    StockfishAnalyzer& operator=(const StockfishAnalyzer&) = delete;
    StockfishAnalyzer(StockfishAnalyzer&&) = delete;
    StockfishAnalyzer& operator=(StockfishAnalyzer&&) = delete;

    void set_multi_pv(int multi_pv);
    StockfishResult analyze_position(const std::string& fen);
    std::vector<StockfishResult> analyze_positions(const std::vector<std::string>& fens);

private:
    struct StockfishImpl;
    StockfishImpl* impl_;
    int multi_pv_;
};

} // namespace aa
