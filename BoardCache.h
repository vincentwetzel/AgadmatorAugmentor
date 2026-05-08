#pragma once

#include "BoardLocalizer.h"
#include <opencv2/opencv.hpp>
#include <string>
#include <memory>
#include <functional>

namespace cta {

class BoardCache {
public:
    static std::unique_ptr<BoardGeometry> load_or_locate(
        const std::string& safe_video_path,
        const cv::Mat& first_frame,
        const cv::Mat& board_template,
        const std::function<void(const std::string&)>& log_info);
};

} // namespace cta