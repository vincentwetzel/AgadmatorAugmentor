#pragma once

#include "ChessVideoExtractor.h"
#include "BoardLocalizer.h"

#include "libchess/move.hpp"
#include "libchess/position.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>
#include <opencv2/videoio.hpp>

namespace cta {

class RevertManager;

struct ChessVideoExtractor::MoveScore {
    int from_sq = -1;
    int to_sq = -1;
    char promotion = '\0';
    double score = 0.0;
};

struct ChessVideoExtractor::ScratchBuffers {
    cv::Mat white_mask;
    cv::Mat reduced;
};

namespace extractor_detail {

double score_to_confidence(double s);
double round_t(double val);

void log_top_candidates(const std::vector<double>& sq_diffs,
                        libchess::Position* pos_ptr,
                        const std::function<void(const std::string&)>& log_info,
                        double elapsed);

cv::VideoCapture open_video_capture(const std::string& safe_video_path);

bool is_inverse_of_recent_move(const std::vector<std::string>& moves,
                               const char* from_name,
                               const char* to_name);

// Returns the most recent matching reverse move in the bounded anti-bounce
// history. Callers can use its timestamp to distinguish a settled take-back
// from a short UI/animation reversal.
std::optional<size_t> find_recent_inverse_move_index(const std::vector<std::string>& moves,
                                                      const char* from_name,
                                                      const char* to_name);

bool passes_yellowness_check(const cv::Mat& board_bgr,
                             const BoardGeometry& geo,
                             const char* from_name,
                             const char* to_name);

double square_piece_edge_score(const cv::Mat& board_bgr,
                               const BoardGeometry& geo,
                               const char* sq_name);

bool is_valid_libchess_move(libchess::Position& pos,
                            const std::string& move_uci,
                            libchess::Move& out_move);

// Resolve a visually adjacent endpoint for a legal horizontal/vertical
// slider move.  The same logic applies to rooks and queens; the helper is
// deliberately based on reusable board/UI evidence rather than a fixture.
void adjust_sliding_target(int& to_sq,
                           const char*& to_name,
                           int from_sq,
                           const char* from_name,
                           const std::vector<double>& sq_diffs,
                           const cv::Mat& board_bgr,
                           const BoardGeometry& geo,
                           libchess::Position* pos_ptr);

} // namespace extractor_detail

} // namespace cta
