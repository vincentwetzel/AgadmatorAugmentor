#pragma once

#include <opencv2/core/mat.hpp>
#include <string>
#include <vector>

namespace aa {

struct BoardGeometry;

// ── Batch 64-square mean computation ─────────────────────────────────────────

/// Computes the mean value for all 64 squares in a single pass using
/// integral image, avoiding 64 separate cv::mean() calls with individual ROIs.
/// @param img  Single-channel image (e.g. absdiff result)
/// @param geo  Board geometry
/// @param margin_h Vertical margin to exclude from each square (edges)
/// @param margin_w Horizontal margin to exclude from each square (edges)
/// @return Vector of 64 mean values (row-major: index = row*8 + col)
std::vector<double> compute_all_square_means(const cv::Mat& img,
                                              const BoardGeometry& geo,
                                              int margin_h,
                                              int margin_w);

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

// ── Hover box detection ──────────────────────────────────────────────────────

/// Finds the square a piece is currently being hovered over (white outline).
/// Returns the square name (e.g. "e4") or empty string if none found.
std::string find_misaligned_piece(const cv::Mat& img_bgr,
                                  const cv::Mat& board_template,
                                  const BoardGeometry& geo);

// ── Debug helpers ────────────────────────────────────────────────────────────

/// Generates an empty board image highlighting the 4 corners tested for yellowness.
void generate_corner_debug_image(const cv::Mat& board_template, const std::string& output_dir);

} // namespace aa
