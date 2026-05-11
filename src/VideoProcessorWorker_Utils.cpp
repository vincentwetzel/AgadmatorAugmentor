#include "VideoProcessorWorker_Utils.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cta {
namespace Utils {


std::string format_clock_string(std::string clockStr) {
    // Standard PGN clock format requires hours: [%clk h:mm:ss]
    if (clockStr.empty()) {
        return "0:00:00"; // Fallback for blank or malformed clocks
    } else if (std::count(clockStr.begin(), clockStr.end(), ':') == 0) {
        // If there are no colons but there is a decimal (e.g. "14.5")
        size_t dot_pos = clockStr.find('.');
        if (dot_pos != std::string::npos) {
            if (dot_pos == 1) {
                return "0:00:0" + clockStr; // e.g., "9.5" -> "0:00:09.5"
            } else {
                return "0:00:" + clockStr; // e.g., "14.5" -> "0:00:14.5"
            }
        } else {
            if (clockStr.length() == 1) {
                return "0:00:0" + clockStr; // e.g., "9" -> "0:00:09"
            } else {
                return "0:00:" + clockStr; // e.g., "45" -> "0:00:45"
            }
        }
    } else if (std::count(clockStr.begin(), clockStr.end(), ':') == 1) {
        if (clockStr.find(':') == 1) {
            return "0:0" + clockStr; // e.g., 9:58 -> 0:09:58
        } else {
            return "0:" + clockStr; // e.g., 10:00 -> 0:10:00
        }
    }
    return clockStr; // Already in h:mm:ss format
}

std::string format_srt_timestamp(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    const long long total_ms = static_cast<long long>(std::llround(seconds * 1000.0));
    const long long ms = total_ms % 1000;
    const long long total_seconds = total_ms / 1000;
    const long long s = total_seconds % 60;
    const long long total_minutes = total_seconds / 60;
    const long long m = total_minutes % 60;
    const long long h = total_minutes / 60;

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(2) << h << ":" << std::setw(2) << m << ":"
        << std::setw(2) << s << "," << std::setw(3) << ms;
    return out.str();
}

std::string move_to_subtitle_text(size_t ply_index, const std::string& san_move) {
    std::ostringstream out;
    const size_t move_number = (ply_index / 2) + 1;
    if (ply_index % 2 == 0) { // White's move
        out << move_number << ". " << san_move;
    } else { // Black's move
        out << move_number << "... " << san_move;
    }
    return out.str();
}

bool is_ffmpeg_available() {
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmd_str = "ffmpeg -version";
    std::vector<char> cmd(cmd_str.begin(), cmd_str.end());
    cmd.push_back('\0');

    if (CreateProcessA(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode == 0;
    }
    return false;
#else
    return std::system("ffmpeg -version > /dev/null 2>&1") == 0;
#endif
}

} // namespace Utils
} // namespace cta