#include "AnalysisVideoGenerator.h"
#include "StockfishAnalyzer.h"
#include "BoardLocalizer.h"
#include "FFmpegFilterGraph.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include <filesystem>
#include <iostream>
#include <cctype>
#include <cstdlib> // For std::system
#include <cstdio>  // For std::remove, std::rename
#include "GPUAccelerator.h"
#include <cmath>
#include <vector>
#include <optional>
#include <iomanip>
#include <fstream>
#include <atomic>
#include <thread>
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aa {

namespace { // Anonymous namespace for helper functions

std::array<char, 64> expand_fen_to_board(const std::string& fen) {
    std::array<char, 64> board;
    board.fill(' ');
    int sq = 56;
    for (char c : fen) {
        if (c == ' ') break;
        if (c == '/') sq -= 16;
        else if (c >= '1' && c <= '8') sq += (c - '0');
        else board[sq++] = c;
    }
    return board;
}

std::string build_san(const libchess::Position& pos, const libchess::Move& move, const std::string& uci_str) {
    auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
    auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));
    
    std::array<char, 64> board = expand_fen_to_board(pos.get_fen());
    char piece = board[from_sq];
    char target_piece = board[to_sq];
    
    bool is_pawn = (piece == 'P' || piece == 'p');
    bool is_capture = (target_piece != ' ') || (is_pawn && (from_sq % 8) != (to_sq % 8) && target_piece == ' ');
    
    if (move.type() == libchess::MoveType::ksc) return "O-O";
    if (move.type() == libchess::MoveType::qsc) return "O-O-O";

    if ((piece == 'K' || piece == 'k') && std::abs((from_sq % 8) - (to_sq % 8)) == 2) {
        if (to_sq % 8 == 6) return "O-O";
        if (to_sq % 8 == 2) return "O-O-O";
    }

    std::string san;
    if (!is_pawn) {
        san += static_cast<char>(std::toupper(piece));
        bool file_conflict = false;
        bool rank_conflict = false;
        bool need_disambiguation = false;
        
        for (const auto& alt_move : pos.legal_moves()) {
            auto alt_from = static_cast<int>(static_cast<unsigned int>(alt_move.from()));
            auto alt_to = static_cast<int>(static_cast<unsigned int>(alt_move.to()));
            
            if (alt_from != from_sq && alt_to == to_sq && board[alt_from] == piece) {
                need_disambiguation = true;
                if (alt_from % 8 == from_sq % 8) file_conflict = true;
                if (alt_from / 8 == from_sq / 8) rank_conflict = true;
            }
        }
        
        if (need_disambiguation) {
            if (!file_conflict) san += static_cast<char>('a' + (from_sq % 8));
            else if (!rank_conflict) san += static_cast<char>('1' + (from_sq / 8));
            else {
                san += static_cast<char>('a' + (from_sq % 8));
                san += static_cast<char>('1' + (from_sq / 8));
            }
        }
    } else {
        if (is_capture) san += static_cast<char>('a' + (from_sq % 8));
    }
    
    if (is_capture) san += "x";
    san += static_cast<char>('a' + (to_sq % 8));
    san += static_cast<char>('1' + (to_sq / 8));
    
    if (uci_str.length() >= 5) {
        san += "=";
        san += static_cast<char>(std::toupper(uci_str[4]));
    }
    
    libchess::Position temp_pos = pos;
    temp_pos.makemove(move);
    if (temp_pos.is_checkmate()) san += "#";
    else if (temp_pos.in_check()) san += "+";

    return san;
}

std::string uci_to_san_line(const std::string& uci_line, const std::string& start_fen) {
    std::istringstream iss(uci_line);
    std::string uci_move;
    std::string san_line;
    try {
        libchess::Position pos(start_fen);
        while (iss >> uci_move) {
            if (!san_line.empty()) san_line += " ";
            libchess::Move m = pos.parse_move(uci_move);
            san_line += build_san(pos, m, uci_move);
            pos.makemove(m);
        }
    } catch (...) {
        return uci_line; // fallback to original string on parsing error
    }
    return san_line.empty() ? uci_line : san_line;
}

void drawEngineArrow(cv::Mat& overlay, cv::Point start, cv::Point end, cv::Scalar color, double squareSize, int thicknessPct) {
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double length = std::sqrt(dx*dx + dy*dy);
    if (length == 0) return;

    double angle = std::atan2(dy, dx);
    double thickness = squareSize * (thicknessPct / 100.0);

    // 1. Circular base on start square
    cv::circle(overlay, start, thickness * 0.8, color, cv::FILLED, cv::LINE_AA);
    
    // 2. Arrowhead Dimensions (Triangle)
    double headLength = thickness * 3.0;
    double headWidth = thickness * 2.5;
    
    // Shorten the shaft so it doesn't peek through the arrowhead sides
    double shaftLength = std::max(0.0, length - headLength * 0.8);
    cv::Point2f shaftEnd(start.x + shaftLength * std::cos(angle), start.y + shaftLength * std::sin(angle));
    
    // 3. Rectangular shaft
    std::vector<cv::Point> shaftPts;
    double pAngle = angle + CV_PI / 2.0; // Perpendicular angle
    
    shaftPts.push_back(cv::Point(static_cast<int>(start.x + (thickness/2) * std::cos(pAngle)), static_cast<int>(start.y + (thickness/2) * std::sin(pAngle))));
    shaftPts.push_back(cv::Point(static_cast<int>(start.x - (thickness/2) * std::cos(pAngle)), static_cast<int>(start.y - (thickness/2) * std::sin(pAngle))));
    shaftPts.push_back(cv::Point(static_cast<int>(shaftEnd.x - (thickness/2) * std::cos(pAngle)), static_cast<int>(shaftEnd.y - (thickness/2) * std::sin(pAngle))));
    shaftPts.push_back(cv::Point(static_cast<int>(shaftEnd.x + (thickness/2) * std::cos(pAngle)), static_cast<int>(shaftEnd.y + (thickness/2) * std::sin(pAngle))));
    cv::fillPoly(overlay, std::vector<std::vector<cv::Point>>{shaftPts}, color, cv::LINE_AA);
    
    // 4. Triangular arrowhead targeting the destination square center
    std::vector<cv::Point> headPts;
    cv::Point2f tip(static_cast<float>(end.x), static_cast<float>(end.y));
    cv::Point2f backCenter(end.x - headLength * std::cos(angle), end.y - headLength * std::sin(angle));
    
    headPts.push_back(cv::Point(static_cast<int>(backCenter.x + (headWidth/2) * std::cos(pAngle)), static_cast<int>(backCenter.y + (headWidth/2) * std::sin(pAngle))));
    headPts.push_back(cv::Point(static_cast<int>(backCenter.x - (headWidth/2) * std::cos(pAngle)), static_cast<int>(backCenter.y - (headWidth/2) * std::sin(pAngle))));
    headPts.push_back(cv::Point(static_cast<int>(tip.x), static_cast<int>(tip.y)));
    cv::fillPoly(overlay, std::vector<std::vector<cv::Point>>{headPts}, color, cv::LINE_AA);
}

void drawMoveAnnotationOnBoard(cv::Mat& img, const std::string& uci, const std::string& sym, double sq_w, double sq_h) {
    if (uci.length() < 4) return;
    std::string clean_sym = sym;
    clean_sym.erase(0, clean_sym.find_first_not_of(" "));
    clean_sym.erase(clean_sym.find_last_not_of(" ") + 1);
    if (clean_sym.empty()) return;

    int to_col = uci[2] - 'a';
    int to_row = 7 - (uci[3] - '1');
    
    if (to_col < 0 || to_col > 7 || to_row < 0 || to_row > 7) return;

    double cx = (to_col + 0.5) * sq_w;
    double cy = (to_row + 0.5) * sq_h;

    int ax = static_cast<int>(cx + sq_w * 0.35);
    int ay = static_cast<int>(cy - sq_h * 0.35);
    int radius = static_cast<int>(sq_w * 0.18);

    cv::Scalar bgColor;
    bool drawStar = false;
    bool drawBook = false;
    bool drawThumbsUp = false;
    
    if (clean_sym == "!!") bgColor = cv::Scalar(80, 175, 75);      // Green (OpenCV uses BGR)
    else if (clean_sym == "!") bgColor = cv::Scalar(215, 130, 40); // Blue
    else if (clean_sym == "*") { bgColor = cv::Scalar(80, 175, 75); drawStar = true; } // Green Star
    else if (clean_sym == "(Good)") { bgColor = cv::Scalar(80, 175, 75); drawThumbsUp = true; } // Green Thumbs Up
    else if (clean_sym == "?") bgColor = cv::Scalar(50, 200, 240); // Yellow
    else if (clean_sym == "X") bgColor = cv::Scalar(100, 100, 255); // Light red
    else if (clean_sym == "??") bgColor = cv::Scalar(40, 40, 200);  // Darker red
    else if (clean_sym == "(Book)") { bgColor = cv::Scalar(150, 180, 200); drawBook = true; } // Tan Book
    else return; 

    cv::circle(img, cv::Point(ax, ay), radius, bgColor, cv::FILLED, cv::LINE_AA);
    cv::circle(img, cv::Point(ax, ay), radius, cv::Scalar(20, 20, 20), 1, cv::LINE_AA); // Dark outline

    if (drawStar) {
        std::vector<cv::Point> star_pts;
        double out_r = radius * 0.65;
        double in_r = out_r * 0.382; // Magic ratio for standard 5-pointed star
        for (int i = 0; i < 10; ++i) {
            double angle = i * CV_PI / 5.0 - CV_PI / 2.0;
            double r = (i % 2 == 0) ? out_r : in_r;
            star_pts.push_back(cv::Point(static_cast<int>(ax + r * std::cos(angle)), static_cast<int>(ay + r * std::sin(angle))));
        }
        cv::fillPoly(img, std::vector<std::vector<cv::Point>>{star_pts}, cv::Scalar(255, 255, 255), cv::LINE_AA);
    } else if (drawBook) {
        std::vector<cv::Point> left_page = {
            cv::Point(ax - radius*0.5, ay - radius*0.25),
            cv::Point(ax - radius*0.05, ay - radius*0.1),
            cv::Point(ax - radius*0.05, ay + radius*0.4),
            cv::Point(ax - radius*0.5, ay + radius*0.25)
        };
        std::vector<cv::Point> right_page = {
            cv::Point(ax + radius*0.5, ay - radius*0.25),
            cv::Point(ax + radius*0.05, ay - radius*0.1),
            cv::Point(ax + radius*0.05, ay + radius*0.4),
            cv::Point(ax + radius*0.5, ay + radius*0.25)
        };
        cv::fillPoly(img, std::vector<std::vector<cv::Point>>{left_page, right_page}, cv::Scalar(255, 255, 255), cv::LINE_AA);
        cv::line(img, cv::Point(ax, ay - radius*0.1), cv::Point(ax, ay + radius*0.4), cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
    } else if (drawThumbsUp) {
        // Rounded silhouette styled to read closer to the chess.com "Good" badge
        // at very small sizes: chunky palm, upright thumb, and soft finger bumps.
        std::vector<cv::Point> palm = {
            cv::Point(static_cast<int>(ax - radius * 0.34), static_cast<int>(ay + radius * 0.32)),
            cv::Point(static_cast<int>(ax - radius * 0.02), static_cast<int>(ay + radius * 0.32)),
            cv::Point(static_cast<int>(ax + radius * 0.18), static_cast<int>(ay + radius * 0.24)),
            cv::Point(static_cast<int>(ax + radius * 0.30), static_cast<int>(ay + radius * 0.08)),
            cv::Point(static_cast<int>(ax + radius * 0.30), static_cast<int>(ay - radius * 0.18)),
            cv::Point(static_cast<int>(ax + radius * 0.15), static_cast<int>(ay - radius * 0.28)),
            cv::Point(static_cast<int>(ax - radius * 0.08), static_cast<int>(ay - radius * 0.28)),
            cv::Point(static_cast<int>(ax - radius * 0.18), static_cast<int>(ay - radius * 0.10)),
            cv::Point(static_cast<int>(ax - radius * 0.28), static_cast<int>(ay + radius * 0.02)),
            cv::Point(static_cast<int>(ax - radius * 0.34), static_cast<int>(ay + radius * 0.18))
        };
        cv::fillConvexPoly(img, palm, cv::Scalar(255, 255, 255), cv::LINE_AA);

        std::vector<cv::Point> thumb = {
            cv::Point(static_cast<int>(ax - radius * 0.24), static_cast<int>(ay - radius * 0.05)),
            cv::Point(static_cast<int>(ax - radius * 0.12), static_cast<int>(ay - radius * 0.12)),
            cv::Point(static_cast<int>(ax - radius * 0.07), static_cast<int>(ay - radius * 0.56)),
            cv::Point(static_cast<int>(ax - radius * 0.20), static_cast<int>(ay - radius * 0.64)),
            cv::Point(static_cast<int>(ax - radius * 0.32), static_cast<int>(ay - radius * 0.56)),
            cv::Point(static_cast<int>(ax - radius * 0.35), static_cast<int>(ay - radius * 0.18))
        };
        cv::fillConvexPoly(img, thumb, cv::Scalar(255, 255, 255), cv::LINE_AA);

        cv::circle(img, cv::Point(static_cast<int>(ax - radius * 0.18), static_cast<int>(ay - radius * 0.60)),
                   std::max(1, static_cast<int>(radius * 0.11)), cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);

        const int fingerRadius = std::max(1, static_cast<int>(radius * 0.12));
        cv::circle(img, cv::Point(static_cast<int>(ax + radius * 0.23), static_cast<int>(ay - radius * 0.14)),
                   fingerRadius, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(img, cv::Point(static_cast<int>(ax + radius * 0.16), static_cast<int>(ay + radius * 0.03)),
                   fingerRadius, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(img, cv::Point(static_cast<int>(ax + radius * 0.07), static_cast<int>(ay + radius * 0.17)),
                   fingerRadius, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);

        cv::line(img,
                 cv::Point(static_cast<int>(ax - radius * 0.07), static_cast<int>(ay - radius * 0.02)),
                 cv::Point(static_cast<int>(ax + radius * 0.18), static_cast<int>(ay - radius * 0.06)),
                 bgColor, 1, cv::LINE_AA);
        cv::line(img,
                 cv::Point(static_cast<int>(ax - radius * 0.03), static_cast<int>(ay + radius * 0.12)),
                 cv::Point(static_cast<int>(ax + radius * 0.14), static_cast<int>(ay + radius * 0.10)),
                 bgColor, 1, cv::LINE_AA);
    } else {
        std::string txt = clean_sym;
        int fontFace = cv::FONT_HERSHEY_SIMPLEX;
        double fontScale = (txt.length() > 1) ? radius * 0.045 : radius * 0.055;
        int thickness = (txt.length() > 1) ? 1 : 2;
        int baseline = 0;
        cv::Size textSize = cv::getTextSize(txt, fontFace, fontScale, thickness, &baseline);
        
        cv::Point textOrg(ax - textSize.width / 2, ay + textSize.height / 2 - 1);
        cv::putText(img, txt, textOrg, fontFace, fontScale, cv::Scalar(255, 255, 255), thickness, cv::LINE_AA);
    }
}

void drawAnalysisBar(cv::Mat& img, cv::Rect rect, double cpScore) {
    // Base dark background (advantage to Black)
    cv::rectangle(img, rect, cv::Scalar(40, 40, 40), cv::FILLED);
    
    // Simplified logistic sigmoid matching standard engine win probability scaling
    double winProb = 1.0 / (1.0 + std::exp(-0.00368208 * cpScore));
    
    // Calculate the height of the light/white portion (advantage to White)
    int whiteHeight = static_cast<int>(rect.height * winProb);
    
    // Draw white portion rising from the bottom up
    cv::Rect whiteRect(rect.x, rect.y + rect.height - whiteHeight, rect.width, whiteHeight);
    cv::rectangle(img, whiteRect, cv::Scalar(240, 240, 240), cv::FILLED);
    
    // Draw a prominent indicator line precisely at the current evaluation point
    int indicatorY = rect.y + rect.height - whiteHeight;
    cv::line(img, cv::Point(rect.x, indicatorY), cv::Point(rect.x + rect.width, indicatorY), cv::Scalar(100, 100, 255), 2, cv::LINE_AA);

    // Overlay a border
    cv::rectangle(img, rect, cv::Scalar(100, 100, 100), 2);
}

std::string format_eval_string(const StockfishLine& line, const std::string& fen) {
    bool is_black_to_move = (fen.find(" b ") != std::string::npos);

    if (line.is_mate) {
        int mate_in = line.mate_in;
        if (is_black_to_move) mate_in = -mate_in;
        
        if (mate_in > 0) {
            return "+M" + std::to_string(mate_in);
        } else {
            return "-M" + std::to_string(-mate_in);
        }
    }

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    double eval_cp = line.centipawns / 100.0;
    if (is_black_to_move) eval_cp = -eval_cp;
    
    if (eval_cp >= 0.0) {
        ss << "+";
    }
    ss << eval_cp;
    return ss.str();
}

double get_line_score_cp(const StockfishLine& line) {
    if (line.is_mate) {
        return (line.mate_in > 0) ? (10000.0 - line.mate_in) : (-10000.0 - line.mate_in);
    }
    return static_cast<double>(line.centipawns);
}

double score_from_analysis(const std::optional<StockfishResult>& analysis, const std::string& fen) {
    if (!analysis.has_value() || analysis->lines.empty()) {
        return 0.0;
    }

    const auto& best_line = analysis->lines[0];
    double score = 0.0;
    if (best_line.is_mate) {
        score = (best_line.mate_in > 0) ? 15000.0 : -15000.0;
    } else {
        score = static_cast<double>(best_line.centipawns);
    }
    
    if (fen.find(" b ") != std::string::npos) {
        score = -score;
    }

    return score;
}

#pragma pack(push, 1)
struct BitmapFileHeader {
    std::uint16_t type = 0x4D42;
    std::uint32_t size = 0;
    std::uint16_t reserved1 = 0;
    std::uint16_t reserved2 = 0;
    std::uint32_t offBits = 54;
};

struct BitmapInfoHeader {
    std::uint32_t size = 40;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint16_t planes = 1;
    std::uint16_t bitCount = 24;
    std::uint32_t compression = 0;
    std::uint32_t sizeImage = 0;
    std::int32_t xPelsPerMeter = 0;
    std::int32_t yPelsPerMeter = 0;
    std::uint32_t clrUsed = 0;
    std::uint32_t clrImportant = 0;
};
#pragma pack(pop)

bool write_bmp_fast(const std::filesystem::path& path, const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC3 || !image.isContinuous()) {
        return false;
    }

    const std::uint32_t row_stride = static_cast<std::uint32_t>(image.cols * 3);
    const std::uint32_t padded_stride = (row_stride + 3u) & ~3u;
    const std::uint32_t image_size = padded_stride * static_cast<std::uint32_t>(image.rows);

    BitmapFileHeader file_header;
    BitmapInfoHeader info_header;
    info_header.width = image.cols;
    info_header.height = image.rows;
    info_header.sizeImage = image_size;
    file_header.size = file_header.offBits + image_size;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    out.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    out.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));

    const std::array<char, 3> padding = {0, 0, 0};
    for (int y = image.rows - 1; y >= 0; --y) {
        const char* row_ptr = reinterpret_cast<const char*>(image.ptr<uchar>(y));
        out.write(row_ptr, row_stride);
        if (padded_stride > row_stride) {
            out.write(padding.data(), padded_stride - row_stride);
        }
    }

    return out.good();
}

void render_main_board_arrows(cv::Mat& image,
                              const std::optional<StockfishResult>& analysis,
                              const std::string& fen,
                              int width, int height,
                              int arrow_thickness_pct) {
    image = cv::Mat::zeros(cv::Size(width, height), CV_8UC3);
    if (!analysis.has_value() || analysis->lines.empty()) return;

    double sq_w = static_cast<double>(width) / 8.0;
    double sq_h = static_cast<double>(height) / 8.0;

    try {
        libchess::Position pos(fen);
        double best_score = get_line_score_cp(analysis->lines.front());

        // Draw worse lines first so best lines render on top
        for (int i = static_cast<int>(analysis->lines.size()) - 1; i >= 0; --i) {
            const auto& line = analysis->lines[i];
            if (line.move_uci.empty() || line.move_uci == "ANNOTATION") continue;
            
            double line_score = get_line_score_cp(line);
            double diff_cp = std::max(0.0, best_score - line_score);
            
            // Weight drops from 1.0 (best) to 0.2 (blunder: 300+ cp worse)
            double weight = std::max(0.2, 1.0 - (diff_cp / 300.0));
            int current_thickness_pct = static_cast<int>(arrow_thickness_pct * weight);

            libchess::Move move = pos.parse_move(line.move_uci);
            auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
            auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));

            int from_row = 7 - (from_sq / 8);
            int from_col = from_sq % 8;
            int to_row = 7 - (to_sq / 8);
            int to_col = to_sq % 8;

            cv::Point start(static_cast<int>((from_col + 0.5) * sq_w), static_cast<int>((from_row + 0.5) * sq_h));
            cv::Point end(static_cast<int>((to_col + 0.5) * sq_w), static_cast<int>((to_row + 0.5) * sq_h));

            cv::Scalar color;
            if (i == 0 || diff_cp <= 10.0) { 
                color = cv::Scalar(122, 153, 122); 
            } else {
                double color_scale = std::max(0.5, weight);
                color = cv::Scalar(112 * color_scale, 128 * color_scale, 144 * color_scale);
            }
            
            drawEngineArrow(image, start, end, color, sq_w, current_thickness_pct);
        }
        
        // Draw graphical move quality annotations on top of the main board
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
                drawMoveAnnotationOnBoard(image, uci, sym, sq_w, sq_h);
            }
        }
    } catch(...) {}
}

} // anonymous namespace

AnalysisVideoGenerator::AnalysisVideoGenerator(const std::string& assets_dir) {
    std::string board_path = assets_dir + "/board/board.png";
    board_template_ = cv::imread(board_path, cv::IMREAD_COLOR);
    
    if (board_template_.empty()) {
        throw std::runtime_error("AnalysisVideoGenerator: Failed to load board asset at " + board_path);
    }
    
    load_piece_assets(assets_dir);
}

void AnalysisVideoGenerator::load_piece_assets(const std::string& assets_dir) {
    // Support both the repo's descriptive filenames and shorter legacy names.
    std::map<char, std::vector<std::string>> piece_files = {
        {'P', {"white/white_pawn.png", "white/P.png"}},
        {'N', {"white/white_knight.png", "white/N.png"}},
        {'B', {"white/white_bishop.png", "white/B.png"}},
        {'R', {"white/white_rook.png", "white/R.png"}},
        {'Q', {"white/white_queen.png", "white/Q.png"}},
        {'K', {"white/white_king.png", "white/K.png"}},
        {'p', {"black/black_pawn.png", "black/p.png"}},
        {'n', {"black/black_knight.png", "black/n.png"}},
        {'b', {"black/black_bishop.png", "black/b.png"}},
        {'r', {"black/black_rook.png", "black/r.png"}},
        {'q', {"black/black_queen.png", "black/q.png"}},
        {'k', {"black/black_king.png", "black/k.png"}}
    };

    for (const auto& [fen_char, candidates] : piece_files) {
        for (const auto& rel_path : candidates) {
            std::string full_path = assets_dir + "/pieces/" + rel_path;
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

    // Draw engine arrows on the debug board
    if (analysis.has_value() && !analysis->lines.empty()) {
        try {
            cv::Mat arrow_overlay = cv::Mat::zeros(board.size(), board.type());
            libchess::Position pos(fen);
            
            double best_score = get_line_score_cp(analysis->lines.front());

            // Draw worse lines first so best lines render on top
            for (int i = static_cast<int>(analysis->lines.size()) - 1; i >= 0; --i) {
                const auto& line = analysis->lines[i];
                if (line.move_uci.empty() || line.move_uci == "ANNOTATION") continue;
                
                double line_score = get_line_score_cp(line);
                double diff_cp = std::max(0.0, best_score - line_score);
                
                // Weight drops from 1.0 (best) to 0.2 (blunder: 300+ cp worse)
                double weight = std::max(0.2, 1.0 - (diff_cp / 300.0));
                int current_thickness_pct = static_cast<int>(arrow_thickness_pct * weight);

                libchess::Move move = pos.parse_move(line.move_uci);
                auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
                auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));

                int from_row = 7 - (from_sq / 8);
                int from_col = from_sq % 8;
                int to_row = 7 - (to_sq / 8);
                int to_col = to_sq % 8;

                cv::Point start(static_cast<int>((from_col + 0.5) * sq_w), static_cast<int>((from_row + 0.5) * sq_h));
                cv::Point end(static_cast<int>((to_col + 0.5) * sq_w), static_cast<int>((to_row + 0.5) * sq_h));

                cv::Scalar color;
                if (i == 0 || diff_cp <= 10.0) { 
                    color = cv::Scalar(122, 153, 122); 
                } else {
                    double color_scale = std::max(0.5, weight);
                    color = cv::Scalar(112 * color_scale, 128 * color_scale, 144 * color_scale);
                }

                drawEngineArrow(arrow_overlay, start, end, color, sq_w, current_thickness_pct);
            }
            cv::addWeighted(arrow_overlay, 0.5, board, 1.0, 0.0, board);
            
            // Draw graphical move quality annotations on top of the debug board
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
                    drawMoveAnnotationOnBoard(board, uci, sym, sq_w, sq_h);
                }
            }
        } catch(...) {
            // Ignore errors if FEN or move is invalid, just don't draw arrows
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
        std::string eval_str = format_eval_string(line, fen);
        std::string text = eval_str + " | " + uci_to_san_line(line.pv_line, fen);

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
    drawAnalysisBar(image, cv::Rect(0, 0, width, height), score_from_analysis(analysis, fen));
}

bool AnalysisVideoGenerator::generate_analysis_video(const std::string& input_video_path, 
                                                     const std::string& output_video_path, 
                                                     const BoardGeometry& geo,
                                                     const std::vector<std::string>& fens,
                                                     const std::vector<double>& timestamps,
                                                     const std::vector<StockfishResult>& stockfish_results,
                                                     int arrow_thickness_pct,
                                                     const VideoOverlayConfig& overlay_config,
                                                     std::atomic<bool>* cancel_flag,
                                                     std::function<void(int, const std::string&)> progress_callback) {
    cv::VideoCapture cap(input_video_path);
    if (!cap.isOpened()) {
        if (progress_callback) progress_callback(-1, "Failed to open input video for analysis video generation.");
        return false;
    }

    double fps = cap.get(cv::CAP_PROP_FPS);
    int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    int total_frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));

    // Close the capture object immediately to save memory. 
    // We will NOT decode the original video in OpenCV. FFmpeg will handle it.
    cap.release(); 

    // Unpack piggybacked codecs from the output_video_path string
    std::string actual_output_path = output_video_path;
    std::string vCodec = "libx264";
    std::string aCodec = "copy";
    std::string resolution = "Source Resolution";
    std::string crf = "23";
    std::string arrows_target = "Debug Board";

    // Dynamically unpack pipe-delimited parameters safely
    size_t pipe_pos = actual_output_path.find('|');
    if (pipe_pos != std::string::npos) {
        std::string options = actual_output_path.substr(pipe_pos + 1);
        actual_output_path = actual_output_path.substr(0, pipe_pos);
        
        std::vector<std::string> tokens;
        size_t start = 0, end;
        while ((end = options.find('|', start)) != std::string::npos) {
            tokens.push_back(options.substr(start, end - start));
            start = end + 1;
        }
        tokens.push_back(options.substr(start));
        
        if (tokens.size() > 0) vCodec = tokens[0];
        if (tokens.size() > 1) aCodec = tokens[1];
        if (tokens.size() > 2) resolution = tokens[2];
        if (tokens.size() > 3) crf = tokens[3];
        if (tokens.size() > 6) arrows_target = tokens[6];
    }

    // Clean up codec UI string (e.g., "libx264 (H.264)" -> "libx264")
    size_t space_idx = vCodec.find(' ');
    if (space_idx != std::string::npos) vCodec = vCodec.substr(0, space_idx);
    
    // Clean up audio codec UI string (e.g., "copy (Original)" -> "copy")
    size_t a_space_idx = aCodec.find(' ');
    if (a_space_idx != std::string::npos) aCodec = aCodec.substr(0, a_space_idx);

    bool draw_debug_arrows = (arrows_target == "Debug Board" || arrows_target == "Both");
    bool draw_main_arrows = (arrows_target == "Main Board" || arrows_target == "Both");

    // Define dynamic overlay dimensions based on user config scale
    int debug_h = static_cast<int>(height * overlay_config.board.scale);
    debug_h += debug_h % 2; // Ensure even dimension
    int debug_w = (board_template_.cols * debug_h) / board_template_.rows;
    debug_w += debug_w % 2; // Ensure even dimension
    int text_w = static_cast<int>(400 * overlay_config.pvText.scale);
    text_w += text_w % 2;
    int bar_w = static_cast<int>(30 * overlay_config.evalBar.scale);
    bar_w += bar_w % 2;
    int safe_height = height + (height % 2); // Ensure even dimension
    int bar_h = static_cast<int>(safe_height * overlay_config.evalBar.scale);
    bar_h += bar_h % 2;

    // Pre-scale assets to target resolution to avoid resizing inside the render loop
    cv::Mat scaled_board;
    cv::resize(board_template_, scaled_board, cv::Size(debug_w, debug_h), 0, 0, cv::INTER_AREA);
    
    std::map<char, cv::Mat> scaled_pieces;
    double sq_w = static_cast<double>(debug_w) / 8.0;
    double sq_h = static_cast<double>(debug_h) / 8.0;
    for (const auto& [c, piece] : piece_assets_) {
        cv::resize(piece, scaled_pieces[c], cv::Size(static_cast<int>(sq_w), static_cast<int>(sq_h)), 0, 0, cv::INTER_AREA);
    }

    // Step 1: Render static images for each move and create FFmpeg concat demuxer files.
    // This drops the workload from O(Frames) (e.g., 36,000) to O(Moves) (e.g., 50),
    // speeding up generation by roughly 1000x and avoiding massive temp video files.
    std::filesystem::path temp_dir = std::filesystem::path(actual_output_path).parent_path() / "temp_overlays";
    std::filesystem::create_directories(temp_dir);

    // RAII cleaner to ensure temp files are wiped even if an exception is thrown or generation fails early
    struct TempCleaner {
        std::filesystem::path dir;
        ~TempCleaner() {
            if (std::filesystem::exists(dir)) {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            }
        }
    } temp_cleaner{temp_dir};

    std::string board_txt_path = (temp_dir / "board.txt").string();
    std::string text_txt_path = (temp_dir / "text.txt").string();
    std::string bar_txt_path = (temp_dir / "bar.txt").string();

    std::ofstream board_txt(board_txt_path);
    std::ofstream text_txt(text_txt_path);
    std::ofstream bar_txt(bar_txt_path);

    std::string main_arrows_txt_path = (temp_dir / "main_arrows.txt").string();
    std::ofstream main_arrows_txt;
    if (draw_main_arrows) main_arrows_txt.open(main_arrows_txt_path);

    size_t num_states = timestamps.size() + 1;
    std::vector<size_t> states_to_render;

    for (size_t i = 0; i < num_states; ++i) {
        double start_t = (i == 0) ? 0.0 : timestamps[i-1];
        double end_t = (i < timestamps.size()) ? timestamps[i] : (total_frames / fps);
        double duration = end_t - start_t;
        
        if (duration <= 0 && i < num_states - 1) continue;

        states_to_render.push_back(i);

        std::string board_img = "board_" + std::to_string(i) + ".bmp";
        std::string text_img = "text_" + std::to_string(i) + ".bmp";
        std::string bar_img = "bar_" + std::to_string(i) + ".bmp";
        std::string main_arrows_img = "main_arrows_" + std::to_string(i) + ".bmp";

        if (overlay_config.board.enabled) {
            board_txt << "file '" << board_img << "'\n";
            board_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";
        }
        if (overlay_config.pvText.enabled) {
            text_txt << "file '" << text_img << "'\n";
            text_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";
        }
        if (overlay_config.evalBar.enabled) {
            bar_txt << "file '" << bar_img << "'\n";
            bar_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";
        }
        
        if (draw_main_arrows) {
            main_arrows_txt << "file '" << main_arrows_img << "'\n";
            main_arrows_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";
        }
    }

    if (!states_to_render.empty()) {
        size_t last_idx = states_to_render.back();
        if (overlay_config.board.enabled) board_txt << "file 'board_" << last_idx << ".bmp'\n";
        if (overlay_config.pvText.enabled) text_txt << "file 'text_" << last_idx << ".bmp'\n";
        if (overlay_config.evalBar.enabled) bar_txt << "file 'bar_" << last_idx << ".bmp'\n";
        if (draw_main_arrows) {
            main_arrows_txt << "file 'main_arrows_" << last_idx << ".bmp'\n";
        }
    }

    board_txt.close();
    text_txt.close();
    bar_txt.close();
    if (draw_main_arrows) main_arrows_txt.close();

    unsigned int hw_threads = std::thread::hardware_concurrency();
    int num_threads = (hw_threads > 0) ? static_cast<int>(hw_threads) : 4;
    num_threads = std::clamp(num_threads, 1, 8);
    num_threads = std::min<int>(num_threads, static_cast<int>(std::max<size_t>(1, states_to_render.size())));
    std::vector<std::thread> threads;
    std::atomic<size_t> current_idx = 0;
    std::atomic<int> completed_count = 0;
    std::atomic<bool> thread_failed = false;
    std::mutex io_mutex;

    int main_arrow_w = geo.bw + (geo.bw % 2);
    int main_arrow_h = geo.bh + (geo.bh % 2);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            try {
                while (true) {
                    if (cancel_flag && *cancel_flag) break;
                    if (thread_failed) break;

                    size_t job_idx = current_idx.fetch_add(1);
                    if (job_idx >= states_to_render.size()) break;
                    
                    size_t i = states_to_render[job_idx];

                    // Get current state
                    const std::string& current_fen = (i < fens.size()) ? fens[i] : "8/8/8/8/8/8/8/8";
                    std::optional<StockfishResult> current_analysis;
                    if (i < stockfish_results.size()) {
                        current_analysis = stockfish_results[i];
                    }

                    // --- Render Analysis Text ---
                    cv::Mat cached_text;
                    if (overlay_config.pvText.enabled) {
                        render_analysis_text(cached_text, current_analysis, current_fen, text_w, debug_h);
                    }

                    // --- Render Debug Board ---
                    std::optional<StockfishResult> board_analysis = draw_debug_arrows ? current_analysis : std::nullopt;
                    cv::Mat cached_board;
                    if (overlay_config.board.enabled) {
                        cached_board = render_board_state(current_fen, board_analysis, arrow_thickness_pct, scaled_board, scaled_pieces);
                    }

                    // --- Render Analysis Bar ---
                    cv::Mat cached_bar;
                    if (overlay_config.evalBar.enabled) {
                        render_analysis_bar(cached_bar, current_analysis, current_fen, bar_w, bar_h);
                    }

                    // --- Render Main Arrows ---
                    cv::Mat cached_main_arrows;
                    if (draw_main_arrows) {
                        render_main_board_arrows(cached_main_arrows, current_analysis, current_fen, main_arrow_w, main_arrow_h, arrow_thickness_pct);
                    }

                    std::string board_img = "board_" + std::to_string(i) + ".bmp";
                    std::string text_img = "text_" + std::to_string(i) + ".bmp";
                    std::string bar_img = "bar_" + std::to_string(i) + ".bmp";
                    std::string main_arrows_img = "main_arrows_" + std::to_string(i) + ".bmp";

                    bool write_ok = true;
                    {
                        // Keep temp image writes efficient and predictable; too many concurrent large writes
                        // can become slower than the CPU rendering work itself.
                        std::lock_guard<std::mutex> lock(io_mutex);
                        if (overlay_config.board.enabled) write_ok &= write_bmp_fast(temp_dir / board_img, cached_board);
                        if (overlay_config.pvText.enabled) write_ok &= write_bmp_fast(temp_dir / text_img, cached_text);
                        if (overlay_config.evalBar.enabled) write_ok &= write_bmp_fast(temp_dir / bar_img, cached_bar);
                        if (write_ok && draw_main_arrows) {
                            write_ok = write_bmp_fast(temp_dir / main_arrows_img, cached_main_arrows);
                        }
                    }
                    if (!write_ok) {
                        throw std::runtime_error("Failed to write temporary overlay bitmaps.");
                    }

                    int c = completed_count.fetch_add(1) + 1;
                    if (progress_callback) {
                        int percent = (c * 80) / states_to_render.size();
                        progress_callback(percent, "Generating analysis overlays: " + std::to_string(percent) + "%");
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Exception in render thread: " << e.what() << "\n";
                thread_failed = true;
            } catch (...) {
                thread_failed = true;
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    if (cancel_flag && *cancel_flag) {
        if (progress_callback) progress_callback(-1, "Analysis video generation cancelled before final composition.");
        return false;
    }
    
    if (thread_failed) {
        if (progress_callback) progress_callback(-1, "Error occurred during parallel overlay generation.");
        return false;
    }

    // Step 2: Have FFmpeg perform the composition
    if (progress_callback) progress_callback(80, "Compositing video streams with FFmpeg...");

    int board_x_pos = static_cast<int>(overlay_config.board.x_percent * std::max(0.0, static_cast<double>(width - debug_w)));
    int board_y_pos = static_cast<int>(overlay_config.board.y_percent * std::max(0.0, static_cast<double>(height - debug_h)));
    board_x_pos -= board_x_pos % 2;
    board_y_pos -= board_y_pos % 2;

    int text_x_pos = static_cast<int>(overlay_config.pvText.x_percent * std::max(0.0, static_cast<double>(width - text_w)));
    int text_y_pos = static_cast<int>(overlay_config.pvText.y_percent * std::max(0.0, static_cast<double>(height - debug_h)));
    text_x_pos -= text_x_pos % 2;
    text_y_pos -= text_y_pos % 2;

    int bar_x_pos = static_cast<int>(overlay_config.evalBar.x_percent * std::max(0.0, static_cast<double>(width - bar_w)));
    int bar_y_pos = static_cast<int>(overlay_config.evalBar.y_percent * std::max(0.0, static_cast<double>(height - bar_h)));
    bar_x_pos -= bar_x_pos % 2;
    bar_y_pos -= bar_y_pos % 2;
    
    int safe_bx = geo.bx - (geo.bx % 2);
    int safe_by = geo.by - (geo.by % 2);

    int stream_idx = 1;
    std::string board_stream, bar_stream, text_stream, arrows_stream;
    std::string input_args = "-y -i \"" + input_video_path + "\" ";

    if (overlay_config.board.enabled) {
        input_args += "-f concat -safe 0 -i \"" + board_txt_path + "\" ";
        board_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (overlay_config.evalBar.enabled) {
        input_args += "-f concat -safe 0 -i \"" + bar_txt_path + "\" ";
        bar_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (overlay_config.pvText.enabled) {
        input_args += "-f concat -safe 0 -i \"" + text_txt_path + "\" ";
        text_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (draw_main_arrows) {
        input_args += "-f concat -safe 0 -i \"" + main_arrows_txt_path + "\" ";
        arrows_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }

    // Compose advanced FFMPEG CPU Filter Graph using the builder
    FFmpegFilterGraph graph;
    std::string current_bg = "[0:v]";
    
    if (draw_main_arrows) {
        graph.add_filter(arrows_stream, "colorkey=black:0.01:0.5", "[main_arrows_alpha]");
        graph.add_filter(current_bg + "[main_arrows_alpha]", "overlay=" + std::to_string(safe_bx) + ":" + std::to_string(safe_by), "[bg_arr]");
        current_bg = "[bg_arr]";
    }

    if (overlay_config.pvText.enabled) {
        graph.add_filter(current_bg, "drawbox=x=" + std::to_string(text_x_pos) + ":y=" + std::to_string(text_y_pos) + ":w=" + std::to_string(text_w) + ":h=" + std::to_string(debug_h) + ":color=black@0.6:t=fill", "[bg_box]");
        graph.add_filter(text_stream, "colorkey=black:0.01:0.5", "[txt_alpha]");
        graph.add_filter("[bg_box][txt_alpha]", "overlay=" + std::to_string(text_x_pos) + ":" + std::to_string(text_y_pos), "[bg_txt]");
        current_bg = "[bg_txt]";
    }

    if (overlay_config.board.enabled) {
        graph.add_filter(current_bg + board_stream, "overlay=" + std::to_string(board_x_pos) + ":" + std::to_string(board_y_pos), "[bg_brd]");
        current_bg = "[bg_brd]";
    }
    
    std::string scale_str = "";
    if (resolution.find("1920x1080") != std::string::npos) scale_str = ",scale=1920:-2";
    else if (resolution.find("1280x720") != std::string::npos) scale_str = ",scale=1280:-2";
    else if (resolution.find("3840x2160") != std::string::npos) scale_str = ",scale=3840:-2";

    if (overlay_config.evalBar.enabled) {
        graph.add_filter(current_bg + bar_stream, "overlay=" + std::to_string(bar_x_pos) + ":" + std::to_string(bar_y_pos) + scale_str + ",format=yuv420p");
    } else {
        std::string final_filter = scale_str.empty() ? "format=yuv420p" : scale_str.substr(1) + ",format=yuv420p";
        graph.add_filter(current_bg, final_filter);
    }
    std::string filter_complex = graph.build();

    std::string ffmpeg_cmd;
    std::string actual_vcodec = vCodec;
    std::string extra_args = "";

    if (vCodec == "libvpx-vp9") {
        extra_args = "-deadline realtime -cpu-used 4 -row-mt 1 -crf " + crf + " -b:v 0";
        if (progress_callback) progress_callback(80, "Using CPU-based FFmpeg (" + actual_vcodec + ")...");
    } else {
        if (GPUAccelerator::is_available()) {
            if (vCodec == "libx264") { actual_vcodec = "h264_nvenc"; extra_args = "-preset p4 -cq " + crf; }
            else if (vCodec == "libx265") { actual_vcodec = "hevc_nvenc"; extra_args = "-preset p4 -cq " + crf; }
            if (progress_callback) progress_callback(80, "Using GPU-accelerated FFmpeg (" + actual_vcodec + ") with CPU filters...");
        } else {
            if (vCodec == "libx264") extra_args = "-preset fast -crf " + crf;
            else if (vCodec == "libx265") extra_args = "-preset fast -crf " + crf;
            if (progress_callback) progress_callback(80, "Using CPU-based FFmpeg (" + actual_vcodec + ")...");
        }
    }

    // Fallback to copy if codec isn't set
    if (aCodec.empty()) aCodec = "copy";

    ffmpeg_cmd = "ffmpeg -threads 0 " + input_args + 
                 "-filter_complex \"" + filter_complex + "\" "
                 "-c:v " + actual_vcodec + " " + extra_args + " -c:a " + aCodec + " \"" + actual_output_path + "\"";

#ifdef _WIN32
    SECURITY_ATTRIBUTES saAttr; 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
        if (progress_callback) progress_callback(-1, "Failed to create pipes for FFmpeg.");
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe; // FFmpeg outputs progress to stderr
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmd_buffer(ffmpeg_cmd.begin(), ffmpeg_cmd.end());
    cmd_buffer.push_back('\0');

    int result = -1;
    // TRUE for bInheritHandles so FFmpeg can use hWritePipe. CREATE_NO_WINDOW strictly prevents the console.
    if (CreateProcessA(NULL, cmd_buffer.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe); // Close write end in parent
        
        char buffer[256];
        DWORD bytesRead;
        std::string output_acc;
        
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            if (cancel_flag && *cancel_flag) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            buffer[bytesRead] = '\0';
            output_acc += buffer;
            
            size_t frame_pos = output_acc.rfind("frame=");
            if (frame_pos != std::string::npos) {
                size_t end_pos = output_acc.find("fps=", frame_pos);
                if (end_pos != std::string::npos) {
                    std::string frame_str = output_acc.substr(frame_pos + 6, end_pos - (frame_pos + 6));
                    try {
                        int frame_num = std::stoi(frame_str);
                        if (total_frames > 0 && progress_callback) {
                            int percent = 80 + (frame_num * 20) / total_frames;
                            percent = std::clamp(percent, 80, 99);
                            progress_callback(percent, "Muxing video: frame " + std::to_string(frame_num) + " / " + std::to_string(total_frames));
                        }
                    } catch (...) {}
                    output_acc = output_acc.substr(end_pos);
                }
            }
            if (output_acc.length() > 1024) {
                output_acc = output_acc.substr(output_acc.length() - 512);
            }
        }
        
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            result = static_cast<int>(exit_code);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);
    } else {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
    }
#else
    ffmpeg_cmd += " 2>&1"; // redirect stderr to stdout to capture it
    FILE* pipe = popen(ffmpeg_cmd.c_str(), "r");
    int result = -1;
    if (pipe) {
        char buffer[256];
        std::string output_acc;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            if (cancel_flag && *cancel_flag) {
                break; // Break loop, pclose will wait.
            }
            output_acc += buffer;
            size_t frame_pos = output_acc.rfind("frame=");
            if (frame_pos != std::string::npos) {
                size_t end_pos = output_acc.find("fps=", frame_pos);
                if (end_pos != std::string::npos) {
                    std::string frame_str = output_acc.substr(frame_pos + 6, end_pos - (frame_pos + 6));
                    try {
                        int frame_num = std::stoi(frame_str);
                        if (total_frames > 0 && progress_callback) {
                            int percent = 80 + (frame_num * 20) / total_frames;
                            percent = std::clamp(percent, 80, 99);
                            progress_callback(percent, "Muxing video: frame " + std::to_string(frame_num) + " / " + std::to_string(total_frames));
                        }
                    } catch (...) {}
                    output_acc = output_acc.substr(end_pos);
                }
            }
            if (output_acc.length() > 1024) output_acc = output_acc.substr(output_acc.length() - 512);
        }
        result = pclose(pipe);
    }
#endif

    if (result == 0) {
        if (progress_callback) progress_callback(100, "Debug video generation complete.");
        return true;
    } else {
        if (progress_callback) progress_callback(-1, "FFmpeg composition failed. Is ffmpeg in your system's PATH?");
        return false;
    }
}

} // namespace aa
