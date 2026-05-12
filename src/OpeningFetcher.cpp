#include "OpeningFetcher.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace cta {
namespace {

std::string url_encode_query_value(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return encoded;
}

void set_winhttp_error(LichessOpening& result, const char* operation) {
#ifdef _WIN32
    result.winhttp_error = GetLastError();
    std::ostringstream oss;
    oss << operation << " failed";
    if (result.winhttp_error != 0) {
        oss << " (WinHTTP error " << result.winhttp_error << ")";
    }
    result.error = oss.str();
#else
    result.error = operation;
#endif
}

} // namespace

OpeningFetcher::OpeningFetcher() : stop_(false), unique_game_reached_(false), done_(false) {
    auto cache_path = std::filesystem::temp_directory_path() / "ChessTubeAnalyzer" / "openings_cache.json";
#ifdef _WIN32
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        cache_path = std::filesystem::path(appdata) / "ChessTubeAnalyzer" / "openings_cache.json";
        free(appdata);
    }
#endif
    cache_path_ = cache_path.string();
    if (std::filesystem::exists(cache_path_)) {
        try {
            std::ifstream ifs(cache_path_);
            nlohmann::json j;
            ifs >> j;
            for (auto& el : j.items()) {
                long long tg = el.value().value("total_games", 0LL);
                if (tg < 0) continue; // Skip corrupted entries from previous 32-bit overflows
                
                std::vector<std::string> top_games;
                if (el.value().contains("top_games")) {
                    for (const auto& g : el.value()["top_games"]) {
                        top_games.push_back(g.get<std::string>());
                    }
                } else if (el.value().contains("game_info")) {
                    std::string gi = el.value().value("game_info", "");
                    if (!gi.empty()) top_games.push_back(gi);
                }

                cache_[el.key()] = {el.value().value("eco", ""), el.value().value("name", ""),
                                    "", tg, 200, 0,
                                    el.value().value("found", false), true,
                                    top_games};
            }
        } catch(...) {}
    }

    thread_ = std::thread(&OpeningFetcher::worker_thread, this);
}

OpeningFetcher::~OpeningFetcher() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }

    try {
        nlohmann::json j;
        for (const auto& [fen, info] : cache_) {
            if (info.api_success) {
                j[fen] = {
                    {"eco", info.eco}, {"name", info.name},
                    {"total_games", info.total_games}, {"found", info.found},
                    {"top_games", info.top_games}
                };
            }
        }
        std::filesystem::create_directories(std::filesystem::path(cache_path_).parent_path());
        std::ofstream ofs(cache_path_);
        ofs << j.dump(4);
    } catch(...) {}
}

void OpeningFetcher::set_api_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    api_token_ = token;
}

bool OpeningFetcher::test_connection(std::string& out_error) {
    LichessOpening result = fetch_from_lichess("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    if (result.http_status == 401 || result.http_status == 403) {
        out_error = "Lichess API returned HTTP " + std::to_string(result.http_status) + 
                    " Unauthorized. Please configure a valid API token in Settings -> Advanced.";
        return false;
    }
    return true;
}

void OpeningFetcher::enqueue_fen(const std::string& fen) {
    if (unique_game_reached_ || done_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(fen);
    cv_.notify_one();
}

LichessOpening OpeningFetcher::get_opening(const std::string& fen) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(fen);
    if (it != cache_.end()) {
        return it->second;
    }
    LichessOpening missing;
    missing.total_games = -1LL; // Use -1 to distinguish "not in cache" from "fetched but failed"
    return missing;
}

void OpeningFetcher::wait_until_done() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return unique_game_reached_ || (queue_.empty() && !active_request_);
        });
    }

    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    done_ = true;
}

LichessOpening OpeningFetcher::fetch_from_lichess(const std::string& fen) {
    LichessOpening result;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = api_token_;
    }

#ifdef _WIN32
    HINTERNET hSession = WinHttpOpen(L"ChessTubeAnalyzer-Extract/1.0 (local extraction pipeline)", 
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, 
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secure_protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure_protocols, sizeof(secure_protocols));

#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD dwDecompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(hSession, WINHTTP_OPTION_DECOMPRESSION, &dwDecompression, sizeof(dwDecompression));
#endif

    HINTERNET hConnect = WinHttpConnect(hSession, L"explorer.lichess.ovh", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        set_winhttp_error(result, "WinHttpConnect");
        WinHttpCloseHandle(hSession);
        return result;
    }

    std::string path = "/lichess?variant=standard&topGames=5&fen=" + url_encode_query_value(fen);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.length()), nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.length()), &wpath[0], wlen);

    LPCWSTR acceptTypes[] = { L"application/json", NULL };
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), 
                                            NULL, WINHTTP_NO_REFERER, 
                                            acceptTypes, 
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        set_winhttp_error(result, "WinHttpOpenRequest");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    std::wstring headers = L"";
    if (!token.empty()) {
        std::string auth_header = "Authorization: Bearer " + token + "\r\n";
        int wlen_hdr = MultiByteToWideChar(CP_UTF8, 0, auth_header.c_str(), -1, nullptr, 0);
        headers.resize(wlen_hdr, 0);
        MultiByteToWideChar(CP_UTF8, 0, auth_header.c_str(), -1, &headers[0], wlen_hdr);
        headers.resize(wlen_hdr - 1); // remove the embedded null terminator included by -1
    }

    if (WinHttpSendRequest(hRequest, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), 
                           headers.empty() ? 0 : static_cast<DWORD>(-1), 
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        
        DWORD dwStatusCode = 0;
        DWORD dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, 
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                            WINHTTP_HEADER_NAME_BY_INDEX, 
                            &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        result.http_status = static_cast<int>(dwStatusCode);

        if (dwStatusCode == 429) {
            result.api_success = false;
            result.total_games = -2LL; // Signal rate limit
            result.error = "rate limited by Lichess";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        if (dwStatusCode == 404) {
            result.api_success = true;
            result.total_games = 0; // Out of book
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        if (dwStatusCode == 200) {
            DWORD size = 0;
            DWORD downloaded = 0;
            std::string response_body;

            do {
                size = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &size)) {
                    set_winhttp_error(result, "WinHttpQueryDataAvailable");
                    break;
                }
                if (size == 0) break;

                char* buffer = new char[size];
                if (!WinHttpReadData(hRequest, (LPVOID)buffer, size, &downloaded)) {
                    set_winhttp_error(result, "WinHttpReadData");
                    delete[] buffer;
                    break;
                }
                response_body.append(buffer, downloaded);
                delete[] buffer;

            } while (size > 0);

            try {
                auto j = nlohmann::json::parse(response_body);
                if (j.contains("white") || j.contains("moves")) {
                    result.api_success = true;
                    if (j.contains("opening") && !j["opening"].is_null()) {
                        result.eco = j["opening"].value("eco", "");
                        result.name = j["opening"].value("name", "");
                        result.found = true;
                    }
                    long long white = j.value("white", 0LL);
                    long long draws = j.value("draws", 0LL);
                    long long black = j.value("black", 0LL);
                    result.total_games = white + draws + black;
                    
                    if (j.contains("topGames") && j["topGames"].is_array()) {
                        for (const auto& game : j["topGames"]) {
                            std::string white_name = "Unknown";
                            if (game.contains("white") && game["white"].is_object()) white_name = game["white"].value("name", "Unknown");
                            std::string black_name = "Unknown";
                            if (game.contains("black") && game["black"].is_object()) black_name = game["black"].value("name", "Unknown");
                            std::string year = game.contains("year") && game["year"].is_number() ? std::to_string(game.value("year", 0)) : "";
                            
                            if (!year.empty() && year != "0") result.top_games.push_back(white_name + " vs " + black_name + " (" + year + ")");
                            else result.top_games.push_back(white_name + " vs " + black_name);
                        }
                    }
                } else {
                    result.error = "unexpected JSON response";
                }
            } catch (...) {
                result.error = "failed to parse JSON response";
            }
        } else {
            std::ostringstream oss;
            oss << "HTTP " << dwStatusCode;
            result.error = oss.str();
        }
    } else {
        set_winhttp_error(result, "WinHTTP request");
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
#endif
    return result;
}

void OpeningFetcher::worker_thread() {
    while (true) {
        std::string fen;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !queue_.empty() || stop_; });
            
            if (unique_game_reached_ || (stop_ && queue_.empty())) {
                cv_.notify_all();
                break;
            }
            
            fen = queue_.front();
            queue_.pop();
            
            // Check cache first to avoid redundant API calls
            if (cache_.find(fen) != cache_.end()) {
                if (cache_[fen].total_games <= 1) {
                    unique_game_reached_ = true;
                    cv_.notify_all();
                    break;
                }
                continue; // Instant lookup, no network delay needed
            }

            active_request_ = true;
        }
        
        LichessOpening info;
        while (!stop_) {
            info = fetch_from_lichess(fen);
            if (info.total_games == -2LL) {
                // 429 Too Many Requests: Lichess enforces a strict cooldown
                for (int i = 0; i < 61; ++i) {
                    if (stop_) break;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            } else {
                break;
            }
        }
        if (stop_) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_request_ = false;
            cv_.notify_all();
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[fen] = info;
            active_request_ = false;
            cv_.notify_all();
        }
        if (info.api_success && info.total_games <= 1) {
            unique_game_reached_ = true;
            cv_.notify_all();
            break;
        }
        // Lichess rate limits to 1 per second generally; buffer to stay safe.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
}

} // namespace cta
