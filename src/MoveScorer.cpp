#include "MoveScorer.h"
#include "ExtractorUtils.h"
#include <array>
#include <cctype>

namespace cta {

ExtractedMoveScore MoveScorer::score_moves_for_board(const libchess::Position& pos, const std::vector<double>& sq_diffs) {
    // Thread-local cache to avoid expanding FEN and generating legal moves on every scoring call
    struct FenCache {
        std::string fen;
        std::array<char, 64> board_map;
        std::vector<libchess::Move> legal_moves;
    };
    static thread_local FenCache fen_cache;

    const std::string& fen = pos.get_fen();
    if (fen_cache.fen != fen) {
        fen_cache.fen = fen;
        fen_cache.board_map = utils::expand_fen(fen);
        fen_cache.legal_moves.clear();
        for (const auto& m : pos.legal_moves()) {
            fen_cache.legal_moves.push_back(m);
        }
    }
    const std::array<char, 64>& board_map = fen_cache.board_map;
    const auto& legal_moves = fen_cache.legal_moves;

    ExtractedMoveScore best;
    for (const auto& move : legal_moves) {
        auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
        auto raw_to = static_cast<int>(static_cast<unsigned int>(move.to()));
        int to_sq = raw_to;

        bool is_castling = (move.type() == libchess::MoveType::ksc || move.type() == libchess::MoveType::qsc);
        if (is_castling) {
            if (from_sq == 4) {
                if (raw_to == 7 || raw_to == 6) to_sq = 6;
                else if (raw_to == 0 || raw_to == 2) to_sq = 2;
            } else if (from_sq == 60) {
                if (raw_to == 63 || raw_to == 62) to_sq = 62;
                else if (raw_to == 56 || raw_to == 58) to_sq = 58;
            }
        } else {
            if ((from_sq == 4 && (to_sq == 6 || to_sq == 2)) || 
                (from_sq == 60 && (to_sq == 62 || to_sq == 58))) {
                char p = board_map[from_sq];
                if (p == 'K' || p == 'k') {
                    is_castling = true;
                }
            }
        }

        double score = sq_diffs[from_sq] + sq_diffs[to_sq];

        if (is_castling) {
            int rook_from = -1, rook_to = -1;
            if (to_sq == 6) { rook_from = 7; rook_to = 5; }
            else if (to_sq == 62) { rook_from = 63; rook_to = 61; }
            else if (to_sq == 2) { rook_from = 0; rook_to = 3; }
            else if (to_sq == 58) { rook_from = 56; rook_to = 59; }
            
            // Subtract a baseline penalty (20.0) for the extra squares to prevent fake castling 
            // (e.g. a normal rook move a1c1) from always beating the rook move due to noise accumulation.
            if (rook_from != -1) score += sq_diffs[rook_from] + sq_diffs[rook_to] - 20.0;
        }

        // En passant: captured pawn on adjacent file, same rank as moving pawn
        if (move.type() == libchess::MoveType::enpassant) {
            int captured_pawn_sq = (to_sq & 7) | (from_sq & 0x38);
            score += sq_diffs[captured_pawn_sq] - 10.0;
        }

        char move_promo = '\0';
        char p = board_map[from_sq];
        if ((p == 'P' && to_sq >= 56) || (p == 'p' && to_sq <= 7)) {
            libchess::Position temp_pos = pos;
            temp_pos.makemove(move);
            std::array<char, 64> board_after = utils::expand_fen(temp_pos.get_fen());
            move_promo = static_cast<char>(std::tolower(board_after[to_sq]));
        }

        // Prefer Queen promotion if scores are equal
        if (score > best.score || (score == best.score && score > 0 && move_promo == 'q' && best.promotion != 'q')) {
            best.from_sq = from_sq;
            best.to_sq = to_sq;
            best.score = score;
            best.promotion = move_promo;
        }
    }
    return best;
}

} // namespace cta
