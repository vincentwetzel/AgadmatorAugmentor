// Extracted from cpp directory
#pragma once

#include "libchess/position.hpp"
#include "BoardAnalysis.h"
#include "ClockRecognizer.h"
#include <vector>
#include <string>
#include <optional> // For optional return values

namespace aa {

struct MoveVerificationResult {
    bool move_found = false;
    libchess::Move best_move;
    double best_score = 0.0;
    std::string debug_info; // For logging purposes
    std::optional<std::string> error_reason; // If move_found is false
};

class MoveVerifier {
public:
    MoveVerifier(const BoardAnalysis& board_analysis, const ClockRecognizer& clock_recognizer);

    MoveVerificationResult verify_and_score_move(
        const libchess::Position& current_pos,
        const std::vector<double>& square_diffs, // 64 square diffs
        const BoardAnalysis::YellowSquareDetectionResult& yellow_squares,
        const BoardAnalysis::HoverBoxDetectionResult& hover_boxes,
        const ClockRecognizer::ClockDetectionResult& clocks,
        const std::vector<libchess::Move>& last_moves // For inverse move filter
    );

private:
    const BoardAnalysis& board_analysis_;
    const ClockRecognizer& clock_recognizer_;

    // Constants for validation
    static constexpr double MIN_MOVE_SCORE_THRESHOLD = 25.0;
    static constexpr double INVERSE_MOVE_OVERRIDE_SCORE = 70.0;
    static constexpr size_t INVERSE_MOVE_HISTORY_DEPTH = 4;

    // Helper methods for validation rules
    bool check_yellow_squares(const libchess::Move& move, const BoardAnalysis::YellowSquareDetectionResult& yellow_squares, std::string& debug_info) const;
    bool check_hover_boxes(const BoardAnalysis::HoverBoxDetectionResult& hover_boxes, std::string& debug_info) const;
    bool check_clock_turn(const libchess::Position& current_pos, const ClockRecognizer::ClockDetectionResult& clocks, std::string& debug_info) const;
    bool check_inverse_move_filter(const libchess::Move& move, const std::vector<libchess::Move>& last_moves, double score, std::string& debug_info) const;

    // Helper to score a single move
    double score_move(const libchess::Move& move, const std::vector<double>& square_diffs, const libchess::Position& current_pos) const;
};

} // namespace aa