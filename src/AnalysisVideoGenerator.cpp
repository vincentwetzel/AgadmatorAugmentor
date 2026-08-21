#include "AnalysisVideoGenerator.h"
#include "BoardLocalizer.h"
#include "FFmpegFilterGraph.h"
#include "libchess/position.hpp"
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
#include <cstring>
#include <mutex>
#include <sstream>
#include <opencv2/imgcodecs.hpp>
#include "ChessFenUtils.h"
#include "ExtractorUtils.h"
#include "ImageWriteUtils.h"
#include "AnalysisVideoRenderUtils.h"
#include "AnalysisVideoGenerator_FFmpeg.h"
#include "FfmpegProcessRunner.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cta {


bool AnalysisVideoGenerator::generate_analysis_video(const std::string& input_video_path, 
                                                     const std::string& output_video_path, 
                                                     const BoardGeometry& geo,
                                                     const std::vector<std::string>& fens,
                                                     const std::vector<double>& timestamps,
                                                     const std::vector<StockfishResult>& stockfish_results,
                                                     const std::vector<std::string>& opening_names,
                                                     int arrow_thickness_pct,
                                                     const VideoOverlayConfig& overlay_config,
                                                     bool include_subtitles,
                                                     int export_threads,
                                                     std::atomic<bool>* cancel_flag,
                                                     std::function<void(int, const std::string&)> progress_callback) {
    // Try FFmpeg hardware acceleration first for quick metadata retrieval.
    cv::VideoCapture cap(input_video_path, cv::CAP_FFMPEG, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
    });
    if (!cap.isOpened()) {
        cap.open(input_video_path, cv::CAP_FFMPEG, {
            cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
        });
    }
    if (!cap.isOpened()) {
        cap.open(input_video_path, cv::CAP_ANY, {
            cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
        });
    }

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
    }

    // Clean up codec UI string (e.g., "libx264 (H.264)" -> "libx264")
    size_t space_idx = vCodec.find(' ');
    if (space_idx != std::string::npos) vCodec = vCodec.substr(0, space_idx);
    
    // Clean up audio codec UI string (e.g., "copy (Original)" -> "copy")
    size_t a_space_idx = aCodec.find(' ');
    if (a_space_idx != std::string::npos) aCodec = aCodec.substr(0, a_space_idx);

    std::string arrows_target = overlay_config.arrowsTarget;
    if (arrows_target.empty()) arrows_target = "Analysis Board";
    if (arrows_target == "Debug Board") arrows_target = "Analysis Board";

    bool draw_debug_arrows = (arrows_target == "Analysis Board" || arrows_target == "Both");
    bool draw_main_arrows = (arrows_target == "Main Board" || arrows_target == "Both");

    // Define dynamic overlay dimensions based on user config scale
    int debug_h = static_cast<int>(height * overlay_config.board.scale);
    debug_h += debug_h % 2; // Ensure even dimension
    int debug_w = (board_template_.cols * debug_h) / board_template_.rows;
    debug_w += debug_w % 2; // Ensure even dimension

    int max_pv_lines = 1;
    for (const auto& result : stockfish_results) {
        int visible_lines = 0;
        for (const auto& line : result.lines) {
            if (line.move_uci != "ANNOTATION") {
                ++visible_lines;
            }
        }
        max_pv_lines = std::max(max_pv_lines, visible_lines);
    }

    int text_w = static_cast<int>(kPvTextBaseWidth * overlay_config.pvText.scale);
    text_w += text_w % 2;
    int text_h = static_cast<int>(kPvTextLineHeight * max_pv_lines * overlay_config.pvText.scale);
    text_h += text_h % 2;
    text_h = std::max(text_h, 2);
    
    // Decode independent X and Y scales from the single unified float (X.XXYYYY)
    double encoded_eval_scale = overlay_config.evalBar.scale;
    double eval_sx = std::round(encoded_eval_scale * 100.0) / 100.0;
    double eval_sy = std::round((encoded_eval_scale - eval_sx) * 10000.0 * 100.0) / 100.0;
    if (eval_sy <= 0.0) eval_sy = 1.0;
    
    int bar_w = static_cast<int>(30 * eval_sx);
    bar_w += bar_w % 2;
    int safe_height = height + (height % 2); // Ensure even dimension
    int bar_h = static_cast<int>(safe_height * eval_sy);
    bar_h += bar_h % 2;

    int opening_w = static_cast<int>(800 * overlay_config.openingText.scale);
    opening_w += opening_w % 2;
    int opening_h = static_cast<int>(40 * overlay_config.openingText.scale);
    opening_h += opening_h % 2;

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
    // The output path originates in Qt as UTF-8.  Use the shared conversion
    // before asking filesystem for its parent so non-ASCII filenames do not
    // get reinterpreted through the Windows ANSI code page.
    std::filesystem::path temp_dir = utils::utf8_to_path(actual_output_path).parent_path() / "temp_overlays";
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
    std::string opening_txt_path = (temp_dir / "opening.txt").string();

    std::ofstream board_txt(board_txt_path);
    std::ofstream text_txt(text_txt_path);
    std::ofstream bar_txt(bar_txt_path);
    std::ofstream opening_txt(opening_txt_path);

    std::string main_arrows_txt_path = (temp_dir / "main_arrows.txt").string();
    std::ofstream main_arrows_txt;
    if (draw_main_arrows) main_arrows_txt.open(main_arrows_txt_path);

    // Ensure classical locale so floating point durations are written with a dot (.), 
    // preventing FFmpeg parsing failures in locales that use commas (,).
    board_txt.imbue(std::locale::classic());
    text_txt.imbue(std::locale::classic());
    bar_txt.imbue(std::locale::classic());
    opening_txt.imbue(std::locale::classic());
    if (draw_main_arrows) main_arrows_txt.imbue(std::locale::classic());

    const size_t num_states = timestamps.size() + 1;
    if (!fens.empty() && fens.size() != num_states) {
        if (progress_callback) {
            progress_callback(-1, "Analysis overlay timing mismatch: FEN state count does not match transition timestamp count.");
        }
        return false;
    }
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
        std::string opening_img = "opening_" + std::to_string(i) + ".bmp";
        std::string main_arrows_img = "main_arrows_" + std::to_string(i) + ".png";

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
        if (overlay_config.openingText.enabled) {
            opening_txt << "file '" << opening_img << "'\n";
            opening_txt << "duration " << std::fixed << std::setprecision(3) << duration << "\n";
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
        if (overlay_config.openingText.enabled) opening_txt << "file 'opening_" << last_idx << ".bmp'\n";
        if (draw_main_arrows) {
            main_arrows_txt << "file 'main_arrows_" << last_idx << ".png'\n";
        }
    }

    board_txt.close();
    text_txt.close();
    bar_txt.close();
    opening_txt.close();
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
                        render_analysis_text(cached_text, current_analysis, current_fen, text_w, text_h);
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
                    
                    cv::Mat cached_opening;
                    if (overlay_config.openingText.enabled) {
                        std::string op_name = (i < opening_names.size()) ? opening_names[i] : "";
                        render_opening_text(cached_opening, op_name, opening_w, opening_h);
                    }

                    // --- Render Main Arrows ---
                    cv::Mat cached_main_arrows;
                    if (draw_main_arrows) {
                        AnalysisVideoRenderUtils::render_main_board_arrows(cached_main_arrows,
                                                                           current_analysis,
                                                                           current_fen,
                                                                           main_arrow_w,
                                                                           main_arrow_h,
                                                                           arrow_thickness_pct,
                                                                           &thumbs_up_icon_);
                    }

                    std::string board_img = "board_" + std::to_string(i) + ".bmp";
                    std::string text_img = "text_" + std::to_string(i) + ".bmp";
                    std::string bar_img = "bar_" + std::to_string(i) + ".bmp";
                    std::string opening_img = "opening_" + std::to_string(i) + ".bmp";
                    std::string main_arrows_img = "main_arrows_" + std::to_string(i) + ".png";

                    bool write_ok = true;
                    // Concurrent file writes to distinct files are safely handled by the OS.
                    // This avoids serializing the multi-threaded render loop.
                    if (overlay_config.board.enabled) write_ok &= ImageWriteUtils::write_bmp_fast(temp_dir / board_img, cached_board);
                    if (overlay_config.pvText.enabled) write_ok &= ImageWriteUtils::write_bmp_fast(temp_dir / text_img, cached_text);
                    if (overlay_config.evalBar.enabled) write_ok &= ImageWriteUtils::write_bmp_fast(temp_dir / bar_img, cached_bar);
                    if (overlay_config.openingText.enabled) write_ok &= ImageWriteUtils::write_bmp_fast(temp_dir / opening_img, cached_opening);
                    if (write_ok && draw_main_arrows) {
                        write_ok = ImageWriteUtils::write_png_rgba(temp_dir / main_arrows_img, cached_main_arrows);
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

    return compose_analysis_video(input_video_path, actual_output_path, geo, overlay_config,
                                  board_txt_path, bar_txt_path, text_txt_path, opening_txt_path, main_arrows_txt_path,
                                  draw_main_arrows, width, height, debug_w, debug_h, text_w, text_h,
                                  bar_w, bar_h, opening_w, opening_h, resolution, vCodec, aCodec, crf,
                                  (total_frames > 0 && fps > 0.0) ? static_cast<double>(total_frames) / fps : 0.0,
                                  include_subtitles,
                                  export_threads,
                                  cancel_flag, progress_callback);
}

void AnalysisVideoGenerator::render_opening_text(cv::Mat& image,
                                                 const std::string& opening_name,
                                                 int width,
                                                 int height) const {
    image = cv::Mat::zeros(cv::Size(width, height), CV_8UC3);
    if (opening_name.empty()) return;
    
    double font_scale = 0.8;
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(opening_name, cv::FONT_HERSHEY_SIMPLEX, font_scale, 2, &baseline);
    while (text_size.width > width - 20 && font_scale > 0.3) {
        font_scale -= 0.05;
        text_size = cv::getTextSize(opening_name, cv::FONT_HERSHEY_SIMPLEX, font_scale, 2, &baseline);
    }
    cv::putText(image, opening_name, cv::Point(10, height / 2 + text_size.height / 2), cv::FONT_HERSHEY_SIMPLEX, font_scale, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
}

} // namespace cta
