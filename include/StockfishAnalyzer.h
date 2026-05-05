#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>

namespace cta {

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
    explicit StockfishAnalyzer(int multi_pv = 1, const std::string& stockfish_path = "", int threads = 0);
    ~StockfishAnalyzer();

    // Delete copy/move
    StockfishAnalyzer(const StockfishAnalyzer&) = delete;
    StockfishAnalyzer& operator=(const StockfishAnalyzer&) = delete;
    StockfishAnalyzer(StockfishAnalyzer&&) = delete;
    StockfishAnalyzer& operator=(StockfishAnalyzer&&) = delete;

    using ProgressCallback = std::function<void(int, int)>; // current, total
    void set_progress_callback(ProgressCallback cb);

    void set_multi_pv(int multi_pv);
    StockfishResult analyze_position(const std::string& fen, int depth, int time_ms = 0, int nodes = 0, std::atomic<bool>* cancel_flag = nullptr);
    std::vector<StockfishResult> analyze_positions(const std::vector<std::string>& fens, int depth, int time_ms = 0, int nodes = 0, std::atomic<bool>* cancel_flag = nullptr);

private:
    struct StockfishImpl;
    StockfishImpl* impl_;
    int multi_pv_;
    int threads_;
    std::string stockfish_path_;
    ProgressCallback progress_callback_;
};

} // namespace cta
