#pragma once

#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include <string>
#include <array>
#include <vector>

namespace cta {
namespace Utils {

std::string format_clock_string(std::string clockStr);
std::string format_srt_timestamp(double seconds);
std::string move_to_subtitle_text(size_t ply_index, const std::string& san_move);
bool is_ffmpeg_available();

} // namespace Utils
} // namespace cta