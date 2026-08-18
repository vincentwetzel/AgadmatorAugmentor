#include "AnalysisVideoGenerator_FFmpeg.h"

#include "FFmpegFilterGraph.h"
#include "FfmpegProcessRunner.h"
#include "GPUAccelerator.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <thread>

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

bool wait_for_output_file(const std::filesystem::path& output_path,
                          std::uintmax_t& output_size,
                          std::error_code& error) {
    // A mapped/network drive can make a just-closed file visible to the
    // filesystem a little after FFmpeg has exited.  Retry briefly before
    // declaring a successful FFmpeg run invalid.
    constexpr int kOutputCheckAttempts = 10;
    constexpr auto kOutputCheckDelay = std::chrono::milliseconds(100);

    for (int attempt = 0; attempt < kOutputCheckAttempts; ++attempt) {
        error.clear();
        if (std::filesystem::exists(output_path, error)) {
            output_size = std::filesystem::file_size(output_path, error);
            if (!error && output_size > 0) return true;
        }

        if (attempt + 1 < kOutputCheckAttempts) {
            std::this_thread::sleep_for(kOutputCheckDelay);
        }
    }

    return false;
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

bool compose_analysis_video(const std::string& input_video_path,
                            const std::string& actual_output_path,
                            const BoardGeometry& geo,
                            const VideoOverlayConfig& overlay_config,
                            const std::string& board_txt_path,
                            const std::string& bar_txt_path,
                            const std::string& text_txt_path,
                            const std::string& opening_txt_path,
                            const std::string& main_arrows_txt_path,
                            bool draw_main_arrows,
                            int width,
                            int height,
                            int debug_w,
                            int debug_h,
                            int text_w,
                            int text_h,
                            int bar_w,
                            int bar_h,
                            int opening_w,
                            int opening_h,
                            const std::string& resolution,
                            const std::string& vCodec,
                            std::string aCodec,
                            const std::string& crf,
                            int num_threads,
                            double total_duration_seconds,
                            std::atomic<bool>* cancel_flag,
                            const std::function<void(int, const std::string&)>& progress_callback) {
    // Step 2: Have FFmpeg perform the composition
    if (progress_callback) progress_callback(80, "Compositing video streams with FFmpeg...");

    // Use one normalized filesystem path for the FFmpeg command and for the
    // post-process validation.  On Windows, constructing a path and calling
    // generic_string() can normalize/convert the path, so checking the raw
    // input string afterward can inspect a different path than FFmpeg wrote.
    const std::filesystem::path output_path(actual_output_path);
    const std::string output_path_arg = output_path.generic_string();

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

    // Keep source cover art.  The filtered [out] stream replaces the source
    // presentation video, so it is not enough to rely on automatic mapping.
    // FFmpeg's uppercase V stream specifier excludes attached pictures;
    // mapping all source video and subtracting 0:V therefore leaves only the
    // source thumbnails without assuming a particular stream index.
    std::string map_args = "-map \"[out]\" -map 0:v -map -0:V -map 0:a? ";
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
                 // The filtered presentation video is always the first
                 // output video; all following video streams are source
                 // attached pictures and must remain stream-copied.
                 "-c:v copy -c:v:0 " + actual_vcodec + " " + extra_args + " -c:a " + aCodec + " " + subtitle_codecs +
                 " -c:t copy -nostats -progress pipe:1 \"" + output_path_arg + "\"";

    std::string ffmpeg_tail;
    int result = FfmpegProcessRunner::run_with_progress(ffmpeg_cmd, total_duration_seconds, cancel_flag, progress_callback, ffmpeg_tail);

    if (result == 0) {
        std::error_code ec;
        std::uintmax_t output_size = 0;
        if (!wait_for_output_file(output_path, output_size, ec)) {
            std::ostringstream validation_error;
            validation_error << "\nError: FFmpeg exited successfully but output file is missing or empty: "
                             << output_path.generic_string();
            if (ec) validation_error << " (" << ec.message() << ")";
            if (progress_callback) progress_callback(-1, compose_ffmpeg_failure_message(result, ffmpeg_tail + validation_error.str()));
            return false;
        }
        if (progress_callback) progress_callback(100, "Analysis video composition complete.");
        return true;
    } else {
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        if (progress_callback) progress_callback(-1, compose_ffmpeg_failure_message(result, ffmpeg_tail));
        return false;
    }
}

} // namespace cta
