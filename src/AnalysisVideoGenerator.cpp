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

namespace aa {

namespace { // Anonymous namespace for helper functions

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
            bool first_line = true;
            for (const auto& line : analysis->lines) {
                if (line.move_uci.empty()) continue;
                
                libchess::Move move = pos.parse_move(line.move_uci);
                auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
                auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));

                int from_row = 7 - (from_sq / 8);
                int from_col = from_sq % 8;
                int to_row = 7 - (to_sq / 8);
                int to_col = to_sq % 8;

                cv::Point start(static_cast<int>((from_col + 0.5) * sq_w), static_cast<int>((from_row + 0.5) * sq_h));
                cv::Point end(static_cast<int>((to_col + 0.5) * sq_w), static_cast<int>((to_row + 0.5) * sq_h));

                // Muted green for best move, slate-blue for others
                cv::Scalar color = first_line ? cv::Scalar(122, 153, 122) : cv::Scalar(112, 128, 144);
                drawEngineArrow(arrow_overlay, start, end, color, sq_w, arrow_thickness_pct);

                first_line = false;
            }
            cv::addWeighted(arrow_overlay, 0.5, board, 1.0, 0.0, board);
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
    bool first_line = true;
    for (const auto& line : analysis->lines) {
        std::string eval_str = format_eval_string(line, fen);
        std::string text = eval_str + " | " + line.pv_line;

        cv::Scalar color = first_line ? cv::Scalar(144, 238, 144) : cv::Scalar(220, 220, 220);
        int thickness = first_line ? 2 : 1;

        cv::putText(image, text, cv::Point(15, text_y_pos), cv::FONT_HERSHEY_SIMPLEX, 0.6, color, thickness, cv::LINE_AA);
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

    // Define overlay dimensions
    int debug_h = static_cast<int>(height * 0.30);
    debug_h += debug_h % 2; // Ensure even dimension
    int debug_w = (board_template_.cols * debug_h) / board_template_.rows;
    debug_w += debug_w % 2; // Ensure even dimension
    int text_w = 400;
    int bar_w = 30; // Widened from 20 to 30 for better visibility
    int safe_height = height + (height % 2); // Ensure even dimension

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
    std::filesystem::path temp_dir = std::filesystem::path(output_video_path).parent_path() / "temp_overlays";
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

        board_txt << "file '" << board_img << "'\n";
        board_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";

        text_txt << "file '" << text_img << "'\n";
        text_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";

        bar_txt << "file '" << bar_img << "'\n";
        bar_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";
    }

    if (!states_to_render.empty()) {
        size_t last_idx = states_to_render.back();
        board_txt << "file 'board_" << last_idx << ".bmp'\n";
        text_txt << "file 'text_" << last_idx << ".bmp'\n";
        bar_txt << "file 'bar_" << last_idx << ".bmp'\n";
    }

    board_txt.close();
    text_txt.close();
    bar_txt.close();

    unsigned int hw_threads = std::thread::hardware_concurrency();
    int num_threads = (hw_threads > 0) ? static_cast<int>(hw_threads) : 4;
    num_threads = std::clamp(num_threads, 1, 8);
    num_threads = std::min<int>(num_threads, static_cast<int>(std::max<size_t>(1, states_to_render.size())));
    std::vector<std::thread> threads;
    std::atomic<size_t> current_idx = 0;
    std::atomic<int> completed_count = 0;
    std::atomic<bool> thread_failed = false;
    std::mutex io_mutex;

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
                    render_analysis_text(cached_text, current_analysis, current_fen, text_w, debug_h);

                    // --- Render Debug Board ---
                    cv::Mat cached_board = render_board_state(current_fen, current_analysis, arrow_thickness_pct, scaled_board, scaled_pieces);

                    // --- Render Analysis Bar ---
                    cv::Mat cached_bar;
                    render_analysis_bar(cached_bar, current_analysis, current_fen, bar_w, safe_height);

                    std::string board_img = "board_" + std::to_string(i) + ".bmp";
                    std::string text_img = "text_" + std::to_string(i) + ".bmp";
                    std::string bar_img = "bar_" + std::to_string(i) + ".bmp";

                    bool write_ok = true;
                    {
                        // Keep temp image writes efficient and predictable; too many concurrent large writes
                        // can become slower than the CPU rendering work itself.
                        std::lock_guard<std::mutex> lock(io_mutex);
                        write_ok = write_bmp_fast(temp_dir / board_img, cached_board) &&
                                   write_bmp_fast(temp_dir / text_img, cached_text) &&
                                   write_bmp_fast(temp_dir / bar_img, cached_bar);
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

    int text_x_pos = std::max(0, width - text_w - debug_w - 20);
    int board_x_pos = std::max(0, width - debug_w - 20);
    int bar_x_pos = std::max(20, geo.bx - bar_w - 20); // Position analysis bar just to the left of the main board
    int y_pos = 20;
    
    // Compose advanced FFMPEG CPU Filter Graph using the builder
    FFmpegFilterGraph graph;
    // 1. Draw a semi-transparent black box over the text area on the base video
    graph.add_filter("[0:v]", "drawbox=x=" + std::to_string(text_x_pos) + ":y=" + std::to_string(y_pos) + ":w=" + std::to_string(text_w) + ":h=" + std::to_string(debug_h) + ":color=black@0.6:t=fill", "[bg_box]");
    // 2. Make the black background of the text video transparent
    graph.add_filter("[3:v]", "colorkey=black:0.01:0.5", "[txt_alpha]");
    // 3. Overlay the floating text onto the background with the box
    graph.add_filter("[bg_box][txt_alpha]", "overlay=" + std::to_string(text_x_pos) + ":" + std::to_string(y_pos), "[bg_txt]");
    // 4. Overlay the debug board
    graph.add_filter("[bg_txt][1:v]", "overlay=" + std::to_string(board_x_pos) + ":" + std::to_string(y_pos), "[bg_brd]");
    // 5. Overlay the analysis bar and set final output format
    graph.add_filter("[bg_brd][2:v]", "overlay=" + std::to_string(bar_x_pos) + ":(H-h)/2,format=yuv420p");
    std::string filter_complex = graph.build();

    std::string input_args = "-y -i \"" + input_video_path + "\" "
                             "-f concat -safe 0 -i \"" + board_txt_path + "\" "
                             "-f concat -safe 0 -i \"" + bar_txt_path + "\" "
                             "-f concat -safe 0 -i \"" + text_txt_path + "\" ";

    std::string ffmpeg_cmd;
    if (GPUAccelerator::is_available()) {
        if (progress_callback) progress_callback(80, "Using GPU-accelerated FFmpeg (NVENC) with CPU filters...");
        ffmpeg_cmd = "ffmpeg -threads 0 " + input_args + 
                     "-filter_complex \"" + filter_complex + "\" "
                     "-c:v h264_nvenc -preset p4 -cq 23 -c:a copy \"" + output_video_path + "\"";
    } else {
        if (progress_callback) progress_callback(80, "Using CPU-based FFmpeg (libx264)...");
        ffmpeg_cmd = "ffmpeg -threads 0 " + input_args + 
                     "-filter_complex \"" + filter_complex + "\" "
                     "-c:v libx264 -preset fast -crf 23 -c:a copy \"" + output_video_path + "\"";
    }

#ifdef _WIN32
    ffmpeg_cmd += " > nul 2>&1";
#else
    ffmpeg_cmd += " > /dev/null 2>&1";
#endif

    int result = std::system(ffmpeg_cmd.c_str());

    if (result == 0) {
        if (progress_callback) progress_callback(100, "Debug video generation complete.");
        return true;
    } else {
        if (progress_callback) progress_callback(-1, "FFmpeg composition failed. Is ffmpeg in your system's PATH?");
        return false;
    }
}

} // namespace aa
