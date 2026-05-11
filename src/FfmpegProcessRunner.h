#pragma once
#include <string>
#include <functional>
#include <atomic>

namespace cta {
class FfmpegProcessRunner {
public:
    static int run_with_progress(const std::string& cmd,
                                 int total_frames,
                                 std::atomic<bool>* cancel_flag,
                                 std::function<void(int, const std::string&)> progress_callback,
                                 std::string& out_tail);
};
} // namespace cta