// Extracted from cpp directory
#include "RevertDetector.h"
#include <algorithm>
#include <iostream> // For debug output, can be replaced with a proper logger

namespace aa {

RevertDetector::RevertDetector() {
    history_.reserve(MAX_HISTORY_SIZE); // Pre-allocate memory
}

std::vector<double> RevertDetector::compute_perceptual_hash(const cv::Mat& board_gray) const {
    std::vector<double> hash(64);
    if (board_gray.empty()) return hash;

    int square_size_w = board_gray.cols / 8;
    int square_size_h = board_gray.rows / 8;

    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            cv::Rect roi(c * square_size_w, r * square_size_h, square_size_w, square_size_h);
            // Ensure ROI is within bounds, especially for the last row/column if not perfectly divisible
            roi.width = std::min(roi.width, board_gray.cols - roi.x);
            roi.height = std::min(roi.height, board_gray.rows - roi.y);

            if (roi.width > 0 && roi.height > 0) {
                cv::Scalar mean = cv::mean(board_gray(roi));
                hash[r * 8 + c] = mean[0];
            } else {
                hash[r * 8 + c] = 0.0; // Or some other default value
            }
        }
    }
    return hash;
}

double RevertDetector::calculate_mean_abs_diff(const cv::Mat& img1, const cv::Mat& img2) const {
    if (img1.empty() || img2.empty() || img1.size() != img2.size() || img1.type() != img2.type()) {
        return std::numeric_limits<double>::max(); // Indicate error or significant difference
    }

    cv::Mat diff;
    cv::absdiff(img1, img2, diff);
    return cv::mean(diff)[0];
}

void RevertDetector::add_history_entry(const cv::Mat& board_gray, const libchess::Position& position,
                                       const libchess::Move& last_move, double timestamp,
                                       const ClockRecognizer::ClockDetectionResult& clocks) {
    if (history_.size() >= MAX_HISTORY_SIZE) {
        history_.erase(history_.begin()); // Remove oldest entry
    }
    history_.push_back({board_gray.clone(), compute_perceptual_hash(board_gray), position, last_move, timestamp, clocks});
}

int RevertDetector::detect_revert(const cv::Mat& current_board_gray, double current_timestamp,
                                  libchess::Position& current_pos,
                                  std::vector<libchess::Move>& move_history,
                                  std::vector<double>& timestamp_history,
                                  std::vector<ClockRecognizer::ClockDetectionResult>& clock_history) {
    if (history_.empty()) {
        return -1;
    }

    std::vector<double> current_hash = compute_perceptual_hash(current_board_gray);

    int best_match_idx = -1;
    double min_full_diff = std::numeric_limits<double>::max();

    // Iterate backwards through history for most recent match
    for (int i = static_cast<int>(history_.size()) - 1; i >= 0; --i) {
        const auto& entry = history_[i];

        // O(1) Perceptual Hash check
        double hash_diff_sum = 0.0;
        for (size_t j = 0; j < 64; ++j) {
            hash_diff_sum += std::abs(current_hash[j] - entry.perceptual_hash[j]);
        }
        double mean_hash_diff = hash_diff_sum / 64.0;

        if (mean_hash_diff < PERCEPTUAL_HASH_DIFF_THRESHOLD) {
            // Potential match, perform full image diff
            double full_diff = calculate_mean_abs_diff(current_board_gray, entry.board_gray);

            if (full_diff < FULL_IMAGE_DIFF_THRESHOLD) {
                // Found a match, this is the revert point
                best_match_idx = i;
                min_full_diff = full_diff; // Keep track of the best match
                // We break here because we want the *most recent* match in history.
                // If we continued, we might find an older match which is not what a "revert" implies.
                break;
            }
        }
    }

    if (best_match_idx != -1) {
        // Revert detected! Roll back the state.
        int moves_rolled_back = (history_.size() - 1) - best_match_idx;
        if (moves_rolled_back > 0) {
            std::cout << "Revert detected! Rolling back " << moves_rolled_back << " moves to ply "
                      << best_match_idx + 1 << " (timestamp: " << history_[best_match_idx].timestamp << "s, diff: " << min_full_diff << ")\n";

            current_pos = history_[best_match_idx].position;
            move_history.resize(best_match_idx);
            timestamp_history.resize(best_match_idx);
            clock_history.resize(best_match_idx);

            // Truncate history_ to the revert point (inclusive)
            history_.resize(best_match_idx + 1);
            return best_match_idx;
        }
    }
    return -1; // No revert
}

void RevertDetector::clear_history() {
    history_.clear();
    history_.reserve(MAX_HISTORY_SIZE);
}

} // namespace aa