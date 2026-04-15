// Extracted from cpp directory
#include "MoveVerifier.h"
#include <algorithm>
#include <iostream> // For debug output, can be replaced with a proper logger

namespace aa {

MoveVerifier::MoveVerifier(const BoardAnalysis& board_analysis, const ClockRecognizer& clock_recognizer)
    : board_analysis_(board_analysis), clock_recognizer_(clock_recognizer) {}

double MoveVerifier::score_move(const libchess::Move& move, const std::vector<double>& square_diffs, const libchess::Position& current_pos) const {
    double score = 0.0;
    if (move.from() < 64 && move.to() < 64) {
        score += square_diffs[static_cast<int>(move.from())];
        score += square_diffs[static_cast<int>(move.to())];
    }

    // Special handling for castling: include rook's squares
    if (move.is_castling()) {
        if (move.to() == libchess::Square::G1) { // White kingside
            score += square_diffs[static_cast<int>(libchess::Square::H1)];
            score += square_diffs[static_cast<int>(libchess::Square::F1)];
        } else if (move.to() == libchess::Square::C1) { // White queenside
            score += square_diffs[static_cast<int>(libchess::Square::A1)];
            score += square_diffs[static_cast<int>(libchess::Square::D1)];
        } else if (move.to() == libchess::Square::G8) { // Black kingside
            score += square_diffs[static_cast<int>(libchess::Square::H8)];
            score += square_diffs[static_cast<int>(libchess::Square::F8)];
        } else if (move.to() == libchess::Square::C8) { // Black queenside
            score += square_diffs[static_cast<int>(libchess::Square::A8)];
            score += square_diffs[static_cast<int>(libchess::Square::D8)];
        }
    }
    // Special handling for en passant: include captured pawn's square
    else if (move.is_enpassant()) {
        libchess::Square captured_pawn_sq;
        if (current_pos.turn() == libchess::Color::White) { // White captures black pawn
            captured_pawn_sq = move.to() - 8; // Black pawn was on rank 5, so 1 rank below destination
        } else { // Black captures white pawn
            captured_pawn_sq = move.to() + 8; // White pawn was on rank 4, so 1 rank above destination
        }
        if (captured_pawn_sq < 64) {
            score += square_diffs[static_cast<int>(captured_pawn_sq)];
        }
    }

    return score;
}

bool MoveVerifier::check_yellow_squares(const libchess::Move& move, const BoardAnalysis::YellowSquareDetectionResult& yellow_squares, std::string& debug_info) const {
    if (!yellow_squares.origin_square.has_value() || !yellow_squares.destination_square.has_value()) {
        debug_info += "Yellow squares not detected. ";
        return false;
    }

    int detected_origin = yellow_squares.origin_square.value();
    int detected_dest = yellow_squares.destination_square.value();

    // Check if the move's origin and destination match the detected yellow squares
    // The order of origin/destination from yellow squares can be arbitrary, so check both ways
    if (!((static_cast<int>(move.from()) == detected_origin && static_cast<int>(move.to()) == detected_dest) ||
          (static_cast<int>(move.from()) == detected_dest && static_cast<int>(move.to()) == detected_origin))) {
        debug_info += "Move (" + libchess::uci(move) + ") does not match yellow squares (" +
                      libchess::square_to_string(static_cast<libchess::Square>(detected_origin)) +
                      libchess::square_to_string(static_cast<libchess::Square>(detected_dest)) + "). ";
        return false;
    }
    return true;
}

bool MoveVerifier::check_hover_boxes(const BoardAnalysis::HoverBoxDetectionResult& hover_boxes, std::string& debug_info) const {
    if (hover_boxes.hover_box_detected) {
        debug_info += "Hover box detected. ";
        return false;
    }
    return true;
}

bool MoveVerifier::check_clock_turn(const libchess::Position& current_pos, const ClockRecognizer::ClockDetectionResult& clocks, std::string& debug_info) const {
    if (!clocks.active_player.has_value()) {
        debug_info += "Active player not detected from clocks. ";
        return false;
    }

    libchess::Color expected_turn = current_pos.turn();
    ClockRecognizer::ActivePlayer detected_active_player = clocks.active_player.value();

    bool turn_matches = false;
    if (expected_turn == libchess::Color::White && detected_active_player == ClockRecognizer::ActivePlayer::White) {
        turn_matches = true;
    } else if (expected_turn == libchess::Color::Black && detected_active_player == ClockRecognizer::ActivePlayer::Black) {
        turn_matches = true;
    }

    if (!turn_matches) {
        debug_info += "Clock turn (" + (detected_active_player == ClockRecognizer::ActivePlayer::White ? "White" : "Black") +
                      ") does not match expected turn (" + (expected_turn == libchess::Color::White ? "White" : "Black") + "). ";
        return false;
    }
    return true;
}

bool MoveVerifier::check_inverse_move_filter(const libchess::Move& move, const std::vector<libchess::Move>& last_moves, double score, std::string& debug_info) const {
    if (score > INVERSE_MOVE_OVERRIDE_SCORE) {
        debug_info += "Inverse move filter overridden by high score. ";
        return true; // High score overrides inverse move filter
    }

    for (size_t i = 0; i < std::min(last_moves.size(), INVERSE_MOVE_HISTORY_DEPTH); ++i) {
        const auto& prev_move = last_moves[last_moves.size() - 1 - i];
        if (move.from() == prev_move.to() && move.to() == prev_move.from()) {
            debug_info += "Move (" + libchess::uci(move) + ") is an inverse of a recent move. ";
            return false; // Reject inverse move
        }
    }
    return true;
}

MoveVerificationResult MoveVerifier::verify_and_score_move(
    const libchess::Position& current_pos,
    const std::vector<double>& square_diffs,
    const BoardAnalysis::YellowSquareDetectionResult& yellow_squares,
    const BoardAnalysis::HoverBoxDetectionResult& hover_boxes,
    const ClockRecognizer::ClockDetectionResult& clocks,
    const std::vector<libchess::Move>& last_moves)
{
    MoveVerificationResult result;
    result.move_found = false;
    result.best_score = 0.0;
    result.debug_info = "";

    // Pre-checks for early rejection
    if (!check_hover_boxes(hover_boxes, result.debug_info)) {
        result.error_reason = "Hover box detected.";
        return result;
    }
    if (!check_clock_turn(current_pos, clocks, result.debug_info)) {
        result.error_reason = "Clock turn mismatch.";
        return result;
    }

    libchess::Move best_candidate_move;
    double max_score = 0.0;
    bool candidate_found = false;

    for (const auto& legal_move : current_pos.legal_moves()) {
        double current_score = score_move(legal_move, square_diffs, current_pos);

        if (current_score > max_score) {
            max_score = current_score;
            best_candidate_move = legal_move;
            candidate_found = true;
        }
    }

    if (!candidate_found) {
        result.error_reason = "No legal move candidate found.";
        return result;
    }

    // Apply post-scoring validations
    result.best_move = best_candidate_move;
    result.best_score = max_score;

    if (max_score < MIN_MOVE_SCORE_THRESHOLD) {
        result.debug_info += "Move score (" + std::to_string(max_score) + ") below threshold. ";
        result.error_reason = "Score too low.";
        return result;
    }
    if (!check_yellow_squares(best_candidate_move, yellow_squares, result.debug_info)) {
        result.error_reason = "Yellow squares mismatch.";
        return result;
    }
    if (!check_inverse_move_filter(best_candidate_move, last_moves, max_score, result.debug_info)) {
        result.error_reason = "Inverse move detected.";
        return result;
    }

    result.move_found = true;
    return result;
}

} // namespace aa