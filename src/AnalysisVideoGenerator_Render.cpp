#include "AnalysisVideoGenerator.h"
#include "AnalysisVideoRenderUtils.h"
#include "ChessFenUtils.h"
#include "libchess/position.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <cctype>

namespace cta {

AnalysisVideoGenerator::AnalysisVideoGenerator(const std::string& assets_dir) {
    std::string board_path = assets_dir + "/reference/board/board.png";
    board_template_ = cv::imread(board_path, cv::IMREAD_COLOR);
    
    if (board_template_.empty()) {
        throw std::runtime_error("AnalysisVideoGenerator: Failed to load board asset at " + board_path);
    }

    std::string thumbs_up_path = assets_dir + "/icons/thumbs-up.png";
    thumbs_up_icon_ = cv::imread(thumbs_up_path, cv::IMREAD_UNCHANGED);
    if (thumbs_up_icon_.empty()) {
        std::cerr << "Warning: Failed to load thumbs up asset at " << thumbs_up_path << std::endl;
    }
    
    load_piece_assets(assets_dir);
}

void AnalysisVideoGenerator::load_piece_assets(const std::string& assets_dir) {
    std::map<char, std::vector<std::string>> piece_files = {
        {'P', {"reference/pieces/white/pawn.png"}},
        {'N', {"reference/pieces/white/knight.png"}},
        {'B', {"reference/pieces/white/bishop.png"}},
        {'R', {"reference/pieces/white/rook.png"}},
        {'Q', {"reference/pieces/white/queen.png"}},
        {'K', {"reference/pieces/white/king.png"}},
        {'p', {"reference/pieces/black/pawn.png"}},
        {'n', {"reference/pieces/black/knight.png"}},
        {'b', {"reference/pieces/black/bishop.png"}},
        {'r', {"reference/pieces/black/rook.png"}},
        {'q', {"reference/pieces/black/queen.png"}},
        {'k', {"reference/pieces/black/king.png"}}
    };

    for (const auto& [fen_char, candidates] : piece_files) {
        for (const auto& rel_path : candidates) {
            std::string full_path = assets_dir + "/" + rel_path;
            // IMREAD_UNCHANGED is vital to keep the 4th alpha channel for transparent pieces.
            cv::Mat piece = cv::imread(full_path, cv::IMREAD_UNCHANGED);
            if (!piece.empty()) {
                piece_assets_[fen_char] = piece;
                break;
            }
        }

        if (!piece_assets_.count(fen_char)) {
            std::cerr << "Warning: Failed to load piece asset for FEN char '" << fen_char << "'" << std::endl;
        }
    }
}

void AnalysisVideoGenerator::overlay_image(cv::Mat& background, const cv::Mat& foreground, cv::Point location) {
    if (foreground.empty()) {
        return;
    }

    cv::Rect roi(location.x, location.y, foreground.cols, foreground.rows);
    // Boundary check
    if (roi.x + roi.width > background.cols || roi.y + roi.height > background.rows || roi.x < 0 || roi.y < 0) {
        return;
    }
    cv::Mat bg_roi = background(roi);

    if (foreground.channels() == 3) {
        // Simple copy for opaque foregrounds (like the final debug board)
        foreground.copyTo(bg_roi);
    } else if (foreground.channels() == 4) {
        // Fast integer-math alpha blending
        // Eliminates dozens of intermediate cv::Mat allocations per piece
        for (int y = 0; y < foreground.rows; ++y) {
            const uchar* fg_ptr = foreground.ptr<uchar>(y);
            uchar* bg_ptr = bg_roi.ptr<uchar>(y);
            for (int x = 0; x < foreground.cols; ++x) {
                uchar alpha = fg_ptr[x * 4 + 3];
                if (alpha == 255) {
                    bg_ptr[x * 3 + 0] = fg_ptr[x * 4 + 0];
                    bg_ptr[x * 3 + 1] = fg_ptr[x * 4 + 1];
                    bg_ptr[x * 3 + 2] = fg_ptr[x * 4 + 2];
                } else if (alpha > 0) {
                    uchar inv_alpha = 255 - alpha;
                    bg_ptr[x * 3 + 0] = (fg_ptr[x * 4 + 0] * alpha + bg_ptr[x * 3 + 0] * inv_alpha) / 255;
                    bg_ptr[x * 3 + 1] = (fg_ptr[x * 4 + 1] * alpha + bg_ptr[x * 3 + 1] * inv_alpha) / 255;
                    bg_ptr[x * 3 + 2] = (fg_ptr[x * 4 + 2] * alpha + bg_ptr[x * 3 + 2] * inv_alpha) / 255;
                }
            }
        }
    }
}

cv::Mat AnalysisVideoGenerator::render_board_state(const std::string& fen, 
                                                   const std::optional<StockfishResult>& analysis, 
                                                   int arrow_thickness_pct,
                                                   const cv::Mat& scaled_board,
                                                   const std::map<char, cv::Mat>& scaled_pieces) {
    cv::Mat board = scaled_board.clone();
    double sq_w = static_cast<double>(board.cols) / 8.0;
    double sq_h = static_cast<double>(board.rows) / 8.0;

    // Draw engine arrows before pieces, matching lichess' analysis-board layering.
    if (analysis.has_value() && !analysis->lines.empty()) {
        try {
            libchess::Position pos(fen);
            
            double best_score = ChessFenUtils::get_line_score_cp(analysis->lines.front());

            // Draw worse lines first so best lines render on top
            for (int i = static_cast<int>(analysis->lines.size()) - 1; i >= 0; --i) {
                const auto& line = analysis->lines[i];
                if (line.move_uci.empty() || line.move_uci == "ANNOTATION") continue;
                
                double line_score = ChessFenUtils::get_line_score_cp(line);
                double diff_cp = std::max(0.0, best_score - line_score);
                
                AnalysisVideoRenderUtils::EngineArrowStyle style = AnalysisVideoRenderUtils::compute_engine_arrow_style(i, diff_cp, arrow_thickness_pct);

                libchess::Move move = pos.parse_move(line.move_uci);
                auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
                auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));

                if (move.type() == libchess::MoveType::ksc || move.type() == libchess::MoveType::qsc) {
                    if (from_sq == 4) {
                        if (to_sq == 7 || to_sq == 6) to_sq = 6;
                        else if (to_sq == 0 || to_sq == 2) to_sq = 2;
                    } else if (from_sq == 60) {
                        if (to_sq == 63 || to_sq == 62) to_sq = 62;
                        else if (to_sq == 56 || to_sq == 58) to_sq = 58;
                    }
                }

                int from_row = 7 - (from_sq / 8);
                int from_col = from_sq % 8;
                int to_row = 7 - (to_sq / 8);
                int to_col = to_sq % 8;

                cv::Point start(static_cast<int>((from_col + 0.5) * sq_w), static_cast<int>((from_row + 0.5) * sq_h));
                cv::Point end(static_cast<int>((to_col + 0.5) * sq_w), static_cast<int>((to_row + 0.5) * sq_h));

                AnalysisVideoRenderUtils::blend_arrow_on_bgr(board, start, end, style, sq_w);
            }
        } catch(...) {
            // Ignore errors if FEN or move is invalid, just don't draw arrows
        }
    }

    int row = 0, col = 0;
    for (char c : fen) {
        if (c == ' ') break; // Stop after piece placement data
        if (c == '/') {
            row++;
            col = 0;
        } else if (std::isdigit(c)) {
            col += (c - '0'); // Skip empty squares
        } else {
            auto it = scaled_pieces.find(c);
            if (it != scaled_pieces.end()) {
                cv::Point loc(static_cast<int>(col * sq_w), static_cast<int>(row * sq_h));
                overlay_image(board, it->second, loc);
            }
            col++;
        }
    }

    if (analysis.has_value()) {
        for (const auto& line : analysis->lines) {
            if (line.move_uci == "ANNOTATION") {
                std::string uci, sym;
                size_t uci_len = 0;
                while (uci_len < line.pv_line.length()) {
                    char c = line.pv_line[uci_len];
                    if ((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8') || c == 'q' || c == 'r' || c == 'b' || c == 'n') uci_len++;
                    else break;
                }
                uci = line.pv_line.substr(0, uci_len);
                sym = line.pv_line.substr(uci_len);
                AnalysisVideoRenderUtils::drawMoveAnnotationOnBoard(board, uci, sym, sq_w, sq_h, &thumbs_up_icon_);
            }
        }
    }

    return board;
}

void AnalysisVideoGenerator::render_analysis_text(cv::Mat& image,
                                                  const std::optional<StockfishResult>& analysis,
                                                  const std::string& fen,
                                                  int width,
                                                  int height) const {
    image = cv::Mat::zeros(cv::Size(width, height), CV_8UC3);

    if (!analysis.has_value()) {
        return;
    }

    int text_y_pos = 30;
    auto lines = analysis->lines;

    // Check for the smuggled annotation line
    if (!lines.empty() && lines.back().move_uci == "ANNOTATION") {
        lines.pop_back(); // Remove it so it doesn't render as an engine line
    }

    bool first_line = true;
    for (const auto& line : lines) {
        std::string eval_str = ChessFenUtils::format_eval_string(line, fen);
        std::string text = eval_str + " | " + ChessFenUtils::uci_to_san_line(line.pv_line, fen);

        cv::Scalar color = first_line ? cv::Scalar(144, 238, 144) : cv::Scalar(220, 220, 220);
        int thickness = first_line ? 2 : 1;

        // Auto-adapt font size so long variations fit within the designated text area
        double font_scale = 0.6;
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
        
        while (text_size.width > width - 30 && font_scale > 0.3) {
            font_scale -= 0.05;
            text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
        }

        cv::putText(image, text, cv::Point(15, text_y_pos), cv::FONT_HERSHEY_SIMPLEX, font_scale, color, thickness, cv::LINE_AA);
        text_y_pos += 25;
        first_line = false;
    }
}

void AnalysisVideoGenerator::render_analysis_bar(cv::Mat& image,
                                                 const std::optional<StockfishResult>& analysis,
                                                 const std::string& fen,
                                                 int width,
                                                 int height) const {
    image = cv::Mat::zeros(cv::Size(width, height), CV_8UC3);
    AnalysisVideoRenderUtils::drawAnalysisBar(image, cv::Rect(0, 0, width, height), ChessFenUtils::score_from_analysis(analysis, fen));
}

} // namespace cta
