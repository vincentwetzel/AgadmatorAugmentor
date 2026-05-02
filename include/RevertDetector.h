// Extracted from cpp directory
#pragma once

#include <opencv2/opencv.hpp>
#include "libchess/position.hpp"
#include "ClockRecognizer.h" // For ClockDetectionResult
#include <vector>
#include <string>
#include <queue> // For history of moves/clocks
#include <numeric> // For std::accumulate

namespace cta {

// Forward declaration for ClockRecognizer::ClockDetectionResult if not fully included
// struct ClockRecognizer::ClockDetectionResult;

struct HistoryEntry {
    cv::Mat board_gray; // Grayscale image of the board
    std::vector<double> perceptual_hash; // For O(1) filtering (64 square means)
    libchess::Position position;
    libchess::Move last_move; // The move that led to this position
    double timestamp;
    ClockRecognizer::ClockDetectionResult clocks;
};

class RevertDetector {
public:
    RevertDetector();

    void add_history_entry(const cv::Mat& board_gray, const libchess::Position& position,
                           const libchess::Move& last_move, double timestamp,
                           const ClockRecognizer::ClockDetectionResult& clocks);

    // Checks for a revert and returns the index in history to revert to.
    // Returns -1 if no revert is detected.
    // If a revert is detected, the provided history vectors are truncated.
    int detect_revert(const cv::Mat& current_board_gray, double current_timestamp,
                      libchess::Position& current_pos,
                      std::vector<libchess::Move>& move_history,
                      std::vector<double>& timestamp_history,
                      std::vector<ClockRecognizer::ClockDetectionResult>& clock_history);

    void clear_history();
    size_t history_size() const { return history_.size(); }

private:
    std::vector<HistoryEntry> history_;
    static constexpr size_t MAX_HISTORY_SIZE = 100; // Keep a reasonable history size
    static constexpr double PERCEPTUAL_HASH_DIFF_THRESHOLD = 5.0; // Mean pixel diff threshold for hash
    static constexpr double FULL_IMAGE_DIFF_THRESHOLD = 10.0; // Mean pixel diff threshold for full image

    // Helper to compute perceptual hash (e.g., mean brightness of 64 squares)
    std::vector<double> compute_perceptual_hash(const cv::Mat& board_gray) const;
    double calculate_mean_abs_diff(const cv::Mat& img1, const cv::Mat& img2) const;
};

} // namespace cta