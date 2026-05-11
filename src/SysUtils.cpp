#include "SysUtils.h"
#include <thread>
#include <algorithm>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace cta {
namespace SysUtils {

int max_hardware_thread_count() {
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    return std::max(1, static_cast<int>(hardware_threads));
}

void set_ffmpeg_threads(int threads) {
    std::string val = std::to_string(threads);
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_THREADS", val.c_str());
#else
    setenv("OPENCV_FFMPEG_THREADS", val.c_str(), 1);
#endif
}

} // namespace SysUtils
} // namespace cta