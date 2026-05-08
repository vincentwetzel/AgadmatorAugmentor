#include "BoardCache.h"
#include "ExtractorUtils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cta {

std::unique_ptr<BoardGeometry> BoardCache::load_or_locate(
    const std::string& safe_video_path,
    const cv::Mat& first_frame,
    const cv::Mat& board_template,
    const std::function<void(const std::string&)>& log_info) {

    std::error_code ec;
    auto fsize = std::filesystem::file_size(safe_video_path, ec);
    auto ftime = std::filesystem::last_write_time(safe_video_path, ec).time_since_epoch().count();

    size_t template_hash = 0;
    if (!board_template.empty()) {
        cv::Mat continuous_tpl = board_template.isContinuous() ? board_template : board_template.clone();
        const uchar* p = continuous_tpl.ptr<uchar>(0);
        size_t len = continuous_tpl.total() * continuous_tpl.elemSize();
        size_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < len; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
        template_hash = hash;
    }

    std::string cache_key = std::filesystem::absolute(safe_video_path).string()
        + "_size=" + std::to_string(fsize)
        + "_time=" + std::to_string(ftime)
        + "_video=" + std::to_string(first_frame.cols) + "x" + std::to_string(first_frame.rows)
        + "_template=" + std::to_string(board_template.cols) + "x" + std::to_string(board_template.rows)
        + "_tplhash=" + std::to_string(template_hash);
    
    std::filesystem::path appdata_dir;
#ifdef _WIN32
    size_t len = 0;
    char* appdata = nullptr;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        appdata_dir = std::filesystem::path(appdata) / "ChessTubeAnalyzer";
        free(appdata);
    }
#endif
    if (appdata_dir.empty()) {
        appdata_dir = std::filesystem::temp_directory_path() / "ChessTubeAnalyzer";
    }
    std::filesystem::path cache_file = appdata_dir / "board_cache.json";
    
    if (std::filesystem::exists(cache_file)) {
        try {
            std::ifstream ifs(cache_file);
            nlohmann::json j;
            ifs >> j;
            if (j.contains(cache_key)) {
                auto& c = j[cache_key];
                int bx = c["bx"], by = c["by"], bw = c["bw"], bh = c["bh"];
                if (bx >= 0 && by >= 0 && bw > 0 && bh > 0 && 
                    (bx + bw) <= first_frame.cols && (by + bh) <= first_frame.rows) {
                    auto geo = std::make_unique<BoardGeometry>();
                    geo->bx = bx; geo->by = by; geo->bw = bw; geo->bh = bh;
                    geo->sq_w = c["sq_w"]; geo->sq_h = c["sq_h"];
                    log_info("Loaded exact board scale from cache (skipped multi-pass search).");
                    return geo;
                } else {
                    log_info("Cached board geometry is out of bounds for current frame. Ignoring cache.");
                }
            }
        } catch (...) {}
    }

    log_info("Performing multi-pass template matching to find exact board scale...");
    auto geo = std::make_unique<BoardGeometry>(locate_board(first_frame, board_template));
    
    try {
        std::filesystem::create_directories(appdata_dir);
        nlohmann::json j;
        if (std::filesystem::exists(cache_file)) { std::ifstream ifs(cache_file); ifs >> j; }
        j[cache_key] = { {"bx", geo->bx}, {"by", geo->by}, {"bw", geo->bw}, {"bh", geo->bh}, {"sq_w", geo->sq_w}, {"sq_h", geo->sq_h} };
        std::ofstream ofs(cache_file); ofs << j.dump(4);
    } catch (...) {}
    
    return geo;
}

} // namespace cta