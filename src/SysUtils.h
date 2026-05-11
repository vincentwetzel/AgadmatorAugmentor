#pragma once

namespace cta {
namespace SysUtils {

int max_hardware_thread_count();
void set_ffmpeg_threads(int threads);

} // namespace SysUtils
} // namespace cta