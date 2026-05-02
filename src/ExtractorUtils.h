#pragma once
#include <string>
#include <filesystem>
#include <array>

namespace cta {
namespace utils {

// Format elapsed seconds as [H:MM:SS.mmm]
std::string ts(double elapsed);

// Pre-computed square name lookup
const char* sq_name(int idx);

// Pre-computes a 64-char array from a FEN string for O(1) piece lookups
std::array<char, 64> expand_fen(const std::string& fen);

// UTF-8 to std::filesystem::path
std::filesystem::path utf8_to_path(const std::string& utf8_str);

// Get safe 8.3 short path on Windows to bypass OpenCV Unicode bugs
std::string get_safe_path(const std::string& utf8_path);

} // namespace utils
} // namespace cta