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
#include "ImageWriteUtils.h"
#include "AnalysisVideoRenderUtils.h"
#include "FfmpegProcessRunner.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cta {

namespace {
std::string compose_ffmpeg_failure_message(int result, const std::string& details) {
    std::ostringstream oss;
    oss << "FFmpeg composition failed";
    if (result >= 0) {
        oss << " (exit code " << result << ")";
    }
    oss << ".";
    if (!details.empty()) {
        oss << " Last FFmpeg output:\n";
        std::istringstream iss(details);
        std::string line;
        while (std::getline(iss, line)) {
            // Ignore benign FFmpeg verbosity that isn't the root cause of failure
            if (line.find("Past duration too large") != std::string::npos ||
                line.find("More than 1000 frames duplicated") != std::string::npos ||
                line.find("Qavg:") != std::string::npos ||
                line.empty()) {
                continue;
            }
            oss << line << "\n";
        }
    } else {
        oss << " No FFmpeg output was captured. Check that ffmpeg is in PATH.";
    }
    return oss.str();
}

struct FilterGraphParams {
    int width;
    int height;
    int debug_w;
    int debug_h;
    int text_w;
    int text_h;
    int bar_w;
    int bar_h;
    int opening_w;
    int opening_h;
    int safe_bx;
    int safe_by;
    bool draw_main_arrows;
    bool is_hw_overlay;
    std::string resolution;
    std::string board_stream;
    std::string bar_stream;
    std::string text_stream;
    std::string opening_stream;
    std::string arrows_stream;
    VideoOverlayConfig overlay_config;
    HardwareAccelerationType hw_type;
};

std::string build_filter_complex_string(const FilterGraphParams& p) {
    int board_x_pos = static_cast<int>(p.overlay_config.board.x_percent * std::max(0.0, static_cast<double>(p.width - p.debug_w)));
    int board_y_pos = static_cast<int>(p.overlay_config.board.y_percent * std::max(0.0, static_cast<double>(p.height - p.debug_h)));
    board_x_pos -= board_x_pos % 2;
    board_y_pos -= board_y_pos % 2;

    int text_x_pos = static_cast<int>(p.overlay_config.pvText.x_percent * std::max(0.0, static_cast<double>(p.width - p.text_w)));
    int text_y_pos = static_cast<int>(p.overlay_config.pvText.y_percent * std::max(0.0, static_cast<double>(p.height - p.text_h)));
    text_x_pos -= text_x_pos % 2;
    text_y_pos -= text_y_pos % 2;

    int bar_x_pos = static_cast<int>(p.overlay_config.evalBar.x_percent * std::max(0.0, static_cast<double>(p.width - p.bar_w)));
    int bar_y_pos = static_cast<int>(p.overlay_config.evalBar.y_percent * std::max(0.0, static_cast<double>(p.height - p.bar_h)));
    bar_x_pos -= bar_x_pos % 2;
    bar_y_pos -= bar_y_pos % 2;

    int opening_x_pos = static_cast<int>(p.overlay_config.openingText.x_percent * std::max(0.0, static_cast<double>(p.width - p.opening_w)));
    int opening_y_pos = static_cast<int>(p.overlay_config.openingText.y_percent * std::max(0.0, static_cast<double>(p.height - p.opening_h)));
    opening_x_pos -= opening_x_pos % 2;
    opening_y_pos -= opening_y_pos % 2;

    FFmpegFilterGraph graph(p.hw_type);
    std::string current_bg = "[0:v]";
    
    if (p.draw_main_arrows) {
        std::string arr_fmt = graph.add_filter({p.arrows_stream}, "format=bgra", "[arr_bgra]");
        graph.add_filter({current_bg, arr_fmt}, "overlay=" + std::to_string(p.safe_bx) + ":" + std::to_string(p.safe_by), "[bg_arr]", p.is_hw_overlay);
        current_bg = "[bg_arr]";
    }
    if (p.overlay_config.pvText.enabled) {
        graph.add_filter({current_bg}, "drawbox=x=" + std::to_string(text_x_pos) + ":y=" + std::to_string(text_y_pos) + ":w=" + std::to_string(p.text_w) + ":h=" + std::to_string(p.text_h) + ":color=black@0.6:t=fill", "[bg_box]");
        std::string txt_fmt = graph.add_filter({p.text_stream}, "colorkey=black:0.01:0.5,format=bgra", "[txt_bgra]");
        graph.add_filter({"[bg_box]", txt_fmt}, "overlay=" + std::to_string(text_x_pos) + ":" + std::to_string(text_y_pos), "[bg_txt]", p.is_hw_overlay);
        current_bg = "[bg_txt]";
    }
    if (p.overlay_config.openingText.enabled) {
        graph.add_filter({current_bg}, "drawbox=x=" + std::to_string(opening_x_pos) + ":y=" + std::to_string(opening_y_pos) + ":w=" + std::to_string(p.opening_w) + ":h=" + std::to_string(p.opening_h) + ":color=black@0.6:t=fill", "[bg_op_box]");
        std::string op_fmt = graph.add_filter({p.opening_stream}, "colorkey=black:0.01:0.5,format=bgra", "[op_bgra]");
        graph.add_filter({"[bg_op_box]", op_fmt}, "overlay=" + std::to_string(opening_x_pos) + ":" + std::to_string(opening_y_pos), "[bg_op]", p.is_hw_overlay);
        current_bg = "[bg_op]";
    }
    if (p.overlay_config.board.enabled) {
        std::string board_fmt = graph.add_filter({p.board_stream}, "format=nv12", "[brd_nv12]");
        graph.add_filter({current_bg, board_fmt}, "overlay=" + std::to_string(board_x_pos) + ":" + std::to_string(board_y_pos), "[bg_brd]", p.is_hw_overlay);
        current_bg = "[bg_brd]";
    }
    
    std::string scale_str = "";
    if (p.resolution.find("1920x1080") != std::string::npos) scale_str = "scale=1920:-2";
    else if (p.resolution.find("1280x720") != std::string::npos) scale_str = "scale=1280:-2";
    else if (p.resolution.find("3840x2160") != std::string::npos) scale_str = "scale=3840:-2";

    if (p.overlay_config.evalBar.enabled) {
        std::string bar_fmt = graph.add_filter({p.bar_stream}, "format=nv12", "[bar_nv12]");
        std::string filter_str = "overlay=" + std::to_string(bar_x_pos) + ":" + std::to_string(bar_y_pos);
        current_bg = graph.add_filter({current_bg, bar_fmt}, filter_str, "[bg_bar]", p.is_hw_overlay);
    }
    if (!scale_str.empty()) {
        current_bg = graph.add_filter({current_bg}, scale_str, "[bg_scaled]", p.is_hw_overlay);
    }

    graph.add_filter({current_bg}, "format=yuv420p", "[out]");
    return graph.build();
}

} // namespace

bool AnalysisVideoGenerator::generate_analysis_video(const std::string& input_video_path, 
                                                     const std::string& output_video_path, 
                                                     const BoardGeometry& geo,
                                                     const std::vector<std::string>& fens,
                                                     const std::vector<double>& timestamps,
                                                     const std::vector<StockfishResult>& stockfish_results,
                                                     const std::vector<std::string>& opening_names,
                                                     int arrow_thickness_pct,
                                                     const VideoOverlayConfig& overlay_config,
                                                     std::atomic<bool>* cancel_flag,
                                                     std::function<void(int, const std::string&)> progress_callback) {
    // Try explicit FFmpeg with specific GPU device to quickly retrieve metadata
    cv::VideoCapture cap(input_video_path, cv::CAP_FFMPEG, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY,
        cv::CAP_PROP_HW_DEVICE, 0
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
                        AnalysisVideoRenderUtils::render_main_board_arrows(cached_main_arrows, current_analysis, current_fen, main_arrow_w, main_arrow_h, arrow_thickness_pct);
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

    // Step 2: Have FFmpeg perform the composition
    if (progress_callback) progress_callback(80, "Compositing video streams with FFmpeg...");

    int safe_bx = geo.bx - (geo.bx % 2);
    int safe_by = geo.by - (geo.by % 2);

    int stream_idx = 1;
    std::string board_stream, bar_stream, text_stream, opening_stream, arrows_stream;

    bool has_nvidia_gpu = GPUAccelerator::is_available();
    bool has_amd_gpu = false;
    bool has_intel_gpu = false;

#ifdef _WIN32
    char sysDir[MAX_PATH];
    if (GetSystemDirectoryA(sysDir, MAX_PATH)) {
        std::string sysDirStr(sysDir);
        if (!has_nvidia_gpu) {
            if (std::filesystem::exists(sysDirStr + "\\nvcuda.dll") ||
                std::filesystem::exists(sysDirStr + "\\nvcuvid.dll")) {
                has_nvidia_gpu = true;
            }
        }
        if (std::filesystem::exists(sysDirStr + "\\amfrt64.dll")) {
            has_amd_gpu = true;
        }
        if (std::filesystem::exists(sysDirStr + "\\libmfxhw64.dll") || 
            std::filesystem::exists(sysDirStr + "\\libmfx64.dll")) {
            has_intel_gpu = true;
        }
    }
#endif

    const bool has_alpha_overlays = draw_main_arrows || overlay_config.pvText.enabled || overlay_config.openingText.enabled;
    const bool use_cuda_filters = has_nvidia_gpu && !has_alpha_overlays;
    bool use_hwaccel = use_cuda_filters;

    std::string hw_init_args = "";
    if (use_cuda_filters) {
        hw_init_args = "-init_hw_device cuda=cuda:0 -filter_hw_device cuda ";
    }

    std::string hwaccel_arg = use_hwaccel ? "-hwaccel auto " : "";
    // Standardize all inputs to generic_string (forward slashes) to prevent escaping bugs in FFmpeg's CLI parser
    std::string input_args_str = "-y " + hw_init_args + hwaccel_arg + "-i \"" + std::filesystem::path(input_video_path).generic_string() + "\" ";

    if (overlay_config.board.enabled) {
        input_args_str += "-f concat -safe 0 -i \"" + board_txt_path + "\" ";
        board_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (overlay_config.evalBar.enabled) {
        input_args_str += "-f concat -safe 0 -i \"" + bar_txt_path + "\" ";
        bar_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (overlay_config.pvText.enabled) {
        input_args_str += "-f concat -safe 0 -i \"" + text_txt_path + "\" ";
        text_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (overlay_config.openingText.enabled) {
        input_args_str += "-f concat -safe 0 -i \"" + opening_txt_path + "\" ";
        opening_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }
    if (draw_main_arrows) {
        input_args_str += "-f concat -safe 0 -i \"" + main_arrows_txt_path + "\" ";
        arrows_stream = "[" + std::to_string(stream_idx++) + ":v]";
    }

    std::string srt_path = std::filesystem::path(actual_output_path + ".srt").generic_string();
    bool has_srt = std::filesystem::exists(srt_path);
    int srt_stream_idx = -1;
    if (has_srt) {
        input_args_str += "-i \"" + srt_path + "\" ";
        srt_stream_idx = stream_idx++;
    }

    HardwareAccelerationType hw_type = HardwareAccelerationType::None;
    if (use_hwaccel) {
        if (use_cuda_filters) hw_type = HardwareAccelerationType::NVDEC_CUDA;
    }

    bool is_hw_overlay = (hw_type == HardwareAccelerationType::NVDEC_CUDA);

    FilterGraphParams fgp;
    fgp.width = width;
    fgp.height = height;
    fgp.debug_w = debug_w;
    fgp.debug_h = debug_h;
    fgp.text_w = text_w;
    fgp.text_h = text_h;
    fgp.bar_w = bar_w;
    fgp.bar_h = bar_h;
    fgp.opening_w = opening_w;
    fgp.opening_h = opening_h;
    fgp.safe_bx = safe_bx;
    fgp.safe_by = safe_by;
    fgp.draw_main_arrows = draw_main_arrows;
    fgp.is_hw_overlay = is_hw_overlay;
    fgp.resolution = resolution;
    fgp.board_stream = board_stream;
    fgp.bar_stream = bar_stream;
    fgp.text_stream = text_stream;
    fgp.opening_stream = opening_stream;
    fgp.arrows_stream = arrows_stream;
    fgp.overlay_config = overlay_config;
    fgp.hw_type = hw_type;

    std::string filter_complex = build_filter_complex_string(fgp);

    std::string ffmpeg_cmd;
    std::string actual_vcodec = vCodec;
    std::string extra_args = "";

    if (vCodec == "libvpx-vp9") {
        extra_args = "-deadline realtime -cpu-used 4 -row-mt 1 -crf " + crf + " -b:v 0";
        if (progress_callback) progress_callback(80, "Using CPU-based FFmpeg (" + actual_vcodec + ")...");
    } else if (vCodec == "h264_nvenc" || vCodec == "hevc_nvenc") {
        extra_args = "-preset p4 -cq " + crf;
        if (progress_callback) progress_callback(80, "Using GPU-accelerated FFmpeg (" + actual_vcodec + ") with CPU filters...");
    } else {
        if (has_nvidia_gpu) {
            if (vCodec == "libx264") { actual_vcodec = "h264_nvenc"; extra_args = "-preset p4 -cq " + crf; }
            else if (vCodec == "libx265") { actual_vcodec = "hevc_nvenc"; extra_args = "-preset p4 -cq " + crf; }
            if (progress_callback) {
                progress_callback(80, "Using NVIDIA GPU FFmpeg (" + actual_vcodec + ") with " + std::string(use_cuda_filters ? "HW filters..." : "CPU filters..."));
            }
        } else if (has_amd_gpu) {
            if (vCodec == "libx264") { actual_vcodec = "h264_amf"; extra_args = "-quality speed -rc cqp -qp_i " + crf + " -qp_p " + crf; }
            else if (vCodec == "libx265") { actual_vcodec = "hevc_amf"; extra_args = "-quality speed -rc cqp -qp_i " + crf + " -qp_p " + crf; }
            if (progress_callback) progress_callback(80, "Using AMD GPU FFmpeg (" + actual_vcodec + ") with CPU filters...");
        } else if (has_intel_gpu) {
            if (vCodec == "libx264") { actual_vcodec = "h264_qsv"; extra_args = "-preset veryfast -global_quality " + crf; }
            else if (vCodec == "libx265") { actual_vcodec = "hevc_qsv"; extra_args = "-preset veryfast -global_quality " + crf; }
            if (progress_callback) progress_callback(80, "Using Intel GPU FFmpeg (" + actual_vcodec + ") with CPU filters...");
        } else {
            if (vCodec == "libx264") extra_args = "-preset fast -crf " + crf;
            else if (vCodec == "libx265") extra_args = "-preset fast -crf " + crf;
            if (progress_callback) progress_callback(80, "Using CPU-based FFmpeg (" + actual_vcodec + ")...");
        }
    }

    // Fallback to copy if codec isn't set
    if (aCodec.empty()) aCodec = "copy";

    std::string map_args = "-map \"[out]\" -map 0:a? ";
    if (has_srt) {
        map_args += "-map " + std::to_string(srt_stream_idx) + ":s ";
    }
    map_args += "-map 0:s? -map 0:t? -map_metadata 0 ";

    std::string subtitle_codecs = "-c:s copy";
    if (has_srt) {
        std::string srt_codec = "copy";
        if (actual_output_path.find(".mp4") != std::string::npos || actual_output_path.find(".mov") != std::string::npos) srt_codec = "mov_text";
        else if (actual_output_path.find(".webm") != std::string::npos) srt_codec = "webvtt";
        else srt_codec = "srt";
        
        subtitle_codecs = "-c:s copy -c:s:0 " + srt_codec;
    }

    ffmpeg_cmd = "ffmpeg -threads 0 " + input_args_str + 
                 "-filter_complex_threads " + std::to_string(num_threads) + " " +
                 "-filter_complex \"" + filter_complex + "\" " +
                 map_args +
                 "-c:v " + actual_vcodec + " " + extra_args + " -c:a " + aCodec + " " + subtitle_codecs + " -c:t copy \"" + std::filesystem::path(actual_output_path).generic_string() + "\"";

    std::string ffmpeg_tail;
    int result = FfmpegProcessRunner::run_with_progress(ffmpeg_cmd, total_frames, cancel_flag, progress_callback, ffmpeg_tail);

    if (result == 0) {
        std::error_code ec;
        if (!std::filesystem::exists(actual_output_path, ec) || std::filesystem::file_size(actual_output_path, ec) == 0) {
            std::filesystem::remove(actual_output_path, ec);
            if (progress_callback) progress_callback(-1, compose_ffmpeg_failure_message(result, ffmpeg_tail + "\nError: FFmpeg exited successfully but output file is missing or empty."));
            return false;
        }
        if (progress_callback) progress_callback(100, "Analysis video composition complete.");
        return true;
    } else {
        std::error_code ec;
        std::filesystem::remove(actual_output_path, ec);
        if (progress_callback) progress_callback(-1, compose_ffmpeg_failure_message(result, ffmpeg_tail));
        return false;
    }
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
