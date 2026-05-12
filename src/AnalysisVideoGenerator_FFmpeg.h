#pragma once

#include "BoardLocalizer.h"
#include "VideoOverlayConfig.h"

#include <atomic>
#include <functional>
#include <string>

namespace cta {

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
                            int total_frames,
                            std::atomic<bool>* cancel_flag,
                            const std::function<void(int, const std::string&)>& progress_callback);

} // namespace cta
