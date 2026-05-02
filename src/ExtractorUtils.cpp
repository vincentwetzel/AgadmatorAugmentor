#include "ExtractorUtils.h"
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace cta {
namespace utils {

std::string ts(double elapsed) {
    int total_ms = static_cast<int>(elapsed * 1000.0);
    int h = total_ms / 3600000;
    int m = (total_ms % 3600000) / 60000;
    int s = (total_ms % 60000) / 1000;
    int ms = total_ms % 1000;
    std::ostringstream oss;
    if (h > 0) oss << h << ":" << std::setfill('0') << std::setw(2);
    oss << m << ":" << std::setfill('0') << std::setw(2) << s << "." << std::setfill('0') << std::setw(3) << ms;
    return "[" + oss.str() + "]";
}

static const char* SQ_NAMES[64] = {
    "a1","b1","c1","d1","e1","f1","g1","h1",
    "a2","b2","c2","d2","e2","f2","g2","h2",
    "a3","b3","c3","d3","e3","f3","g3","h3",
    "a4","b4","c4","d4","e4","f4","g4","h4",
    "a5","b5","c5","d5","e5","f5","g5","h5",
    "a6","b6","c6","d6","e6","f6","g6","h6",
    "a7","b7","c7","d7","e7","f7","g7","h7",
    "a8","b8","c8","d8","e8","f8","g8","h8"
};

const char* sq_name(int idx) {
    return SQ_NAMES[idx];
}

std::array<char, 64> expand_fen(const std::string& fen) {
    std::array<char, 64> board;
    board.fill(' ');
    int sq = 56; // Start at a8 (index 56)
    for (char c : fen) {
        if (c == ' ') break; // End of board layout
        if (c == '/') { sq -= 16; } // Move down a rank (e.g., from rank 8 to 7)
        else if (c >= '1' && c <= '8') { sq += (c - '0'); } // Skip empty squares
        else { board[sq++] = c; }
    }
    return board;
}

std::filesystem::path utf8_to_path(const std::string& utf8_str) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return std::filesystem::path(utf8_str);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, &wpath[0], wlen);
    while (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    return std::filesystem::path(wpath);
#else
    return std::filesystem::path(utf8_str);
#endif
}

std::string get_safe_path(const std::string& utf8_path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return utf8_path;
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, &wpath[0], wlen);
    DWORD short_len = GetShortPathNameW(wpath.c_str(), nullptr, 0);
    if (short_len == 0) return utf8_path;
    std::wstring short_wpath(short_len, 0);
    GetShortPathNameW(wpath.c_str(), &short_wpath[0], short_len);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, short_wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return utf8_path;
    std::string short_path(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, short_wpath.c_str(), -1, &short_path[0], ulen, nullptr, nullptr);
    while (!short_path.empty() && short_path.back() == '\0') {
        short_path.pop_back();
    }
    return short_path;
#else
    return utf8_path;
#endif
}

} // namespace utils
} // namespace cta