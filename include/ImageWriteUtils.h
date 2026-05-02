#pragma once

#include <string>
#include <filesystem>
#include <cstdint>
#include <opencv2/core.hpp>

namespace cta {
namespace ImageWriteUtils {

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

bool write_bmp_fast(const std::filesystem::path& path, const cv::Mat& image);
bool write_png_rgba(const std::filesystem::path& path, const cv::Mat& image);

} // namespace ImageWriteUtils
} // namespace cta
