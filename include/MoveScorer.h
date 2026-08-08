#pragma once

#include "libchess/position.hpp"
#include <vector>

namespace cta {

struct ExtractedMoveScore {
    int from_sq = -1;
    int to_sq = -1;
    char promotion = '\0';
    double score = 0.0;
};

class MoveScorer {
public:
    static ExtractedMoveScore score_moves_for_board(const libchess::Position& pos, const std::vector<double>& sq_diffs);
};

} // namespace cta
