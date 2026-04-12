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
    bool ocr_skipped = false;  // true if times were reused from cache (no OCR ran)
};

/// Cache for conditional clock OCR — holds previous clock ROI grayscale images.
/// When clock pixels haven't meaningfully changed, OCR is skipped and cached
/// times are reused, saving expensive Tesseract calls.
struct ClockCache {
    cv::Mat top_gray;    // Previous top clock ROI (grayscale)
    cv::Mat bot_gray;    // Previous bottom clock ROI (grayscale)
    std::string white_time;
    std::string black_time;
    bool valid = false;
};

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
/// @param cache  Optional cache for conditional OCR. If provided and clock
///               pixels haven't meaningfully changed, OCR is skipped and
///               cached times are reused. Cache is updated on OCR runs.
ClockState extract_clocks(const cv::Mat& img_bgr,
                          const cv::Mat& board_template,
                          const BoardGeometry& geo,
                          ClockCache* cache = nullptr);

// ── Debug helpers ────────────────────────────────────────────────────────────

/// Generates an empty board image highlighting the 4 corners tested for yellowness.
void generate_corner_debug_image(const cv::Mat& board_template, const std::string& output_dir);

} // namespace aa
