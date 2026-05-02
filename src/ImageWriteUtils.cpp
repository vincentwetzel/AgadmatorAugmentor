#include "ImageWriteUtils.h"
#include <fstream>
#include <array>
#include <opencv2/imgcodecs.hpp>

namespace cta {
namespace ImageWriteUtils {

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

bool write_png_rgba(const std::filesystem::path& path, const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC4) {
        return false;
    }

    try {
        return cv::imwrite(path.string(), image);
    } catch (...) {
        return false;
    }
}

} // namespace ImageWriteUtils
} // namespace cta
