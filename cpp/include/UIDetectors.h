#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace aa {

struct BoardGeometry;

/// Result of clock extraction.
struct ClockState {
    std::string active_player; // "white", "black", or empty string if neither
    std::string white_time;    // e.g. "10:00" or "1:31:28"
    std::string black_time;
};

// ── Yellow square move detection ─────────────────────────────────────────────

/// Analyzes a static image for 2 yellow squares to deduce the previous move.
/// Returns the move in UCI notation (e.g. "e2e4").
std::string extract_move_from_yellow_squares(const cv::Mat& img_bgr,
                                              const cv::Mat& board_template,
                                              const BoardGeometry& geo);

// ── Piece counting ───────────────────────────────────────────────────────────

/// Counts the number of chess pieces on the board via Canny edge detection.
int count_pieces_in_image(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo);

// ── Red square detection ─────────────────────────────────────────────────────

/// Finds all squares highlighted in red. Returns sorted list of square names (e.g. {"e4", "d5"}).
std::vector<std::string> find_red_squares(const cv::Mat& img_bgr,
                                          const cv::Mat& board_template,
                                          const cv::Mat& red_board_template,
                                          const BoardGeometry& geo);

// ── Yellow arrow detection ───────────────────────────────────────────────────

/// Finds yellow arrows drawn on the board. Returns sorted list of arrow strings (e.g. {"e2e4", "f1b5"}).
std::vector<std::string> find_yellow_arrows(const cv::Mat& img_bgr,
                                            const cv::Mat& board_template,
                                            const BoardGeometry& geo);

// ── Hover box detection ──────────────────────────────────────────────────────

/// Finds the square a piece is currently being hovered over (white outline).
/// Returns the square name (e.g. "e4") or empty string if none found.
std::string find_misaligned_piece(const cv::Mat& img_bgr,
                                  const cv::Mat& board_template,
                                  const BoardGeometry& geo);

// ── Clock extraction ─────────────────────────────────────────────────────────

/// Extracts the active player and remaining time from both clock pills.
ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo);

// ── Debug helpers ────────────────────────────────────────────────────────────

/// Generates an empty board image highlighting the 4 corners tested for yellowness.
void generate_corner_debug_image(const cv::Mat& board_template, const std::string& output_dir);

} // namespace aa
