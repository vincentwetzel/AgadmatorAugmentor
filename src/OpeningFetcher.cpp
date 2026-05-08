#include "OpeningFetcher.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace cta {

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
                cache_[el.key()] = {el.value().value("eco", ""), el.value().value("name", ""),
                                    el.value().value("total_games", 0), el.value().value("found", false), true};
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
                    {"total_games", info.total_games}, {"found", info.found}
                };
            }
        }
        std::filesystem::create_directories(std::filesystem::path(cache_path_).parent_path());
        std::ofstream ofs(cache_path_);
        ofs << j.dump(4);
    } catch(...) {}
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
    return {};
}

void OpeningFetcher::wait_until_done() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    done_ = true;
}

LichessOpening OpeningFetcher::fetch_from_lichess(const std::string& fen) {
    LichessOpening result;
#ifdef _WIN32
    HINTERNET hSession = WinHttpOpen(L"ChessTubeAnalyzer/1.0", 
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, 
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, L"explorer.lichess.ovh", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    std::string path = "/master?fen=" + fen;
    size_t pos = 0;
    while ((pos = path.find(' ', pos)) != std::string::npos) {
        path.replace(pos, 1, "%20");
        pos += 3;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), 
                                            NULL, WINHTTP_NO_REFERER, 
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        
        DWORD size = 0;
        DWORD downloaded = 0;
        std::string response_body;

        do {
            size = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &size)) break;
            if (size == 0) break;

            char* buffer = new char[size + 1];
            if (!WinHttpReadData(hRequest, (LPVOID)buffer, size, &downloaded)) {
                delete[] buffer;
                break;
            }
            buffer[downloaded] = '\0';
            response_body += buffer;
            delete[] buffer;

        } while (size > 0);

        try {
            auto j = nlohmann::json::parse(response_body);
            result.api_success = true;
            if (j.contains("opening") && !j["opening"].is_null()) {
                result.eco = j["opening"].value("eco", "");
                result.name = j["opening"].value("name", "");
                result.found = true;
            }
            int white = j.value("white", 0);
            int draws = j.value("draws", 0);
            int black = j.value("black", 0);
            result.total_games = white + draws + black;
        } catch (...) {
        }
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
                break;
            }
            
            fen = queue_.front();
            queue_.pop();
            
            // Check cache first to avoid redundant API calls
            if (cache_.find(fen) != cache_.end()) {
                if (cache_[fen].total_games <= 1) {
                    unique_game_reached_ = true;
                    break;
                }
                continue; // Instant lookup, no network delay needed
            }
        }
        LichessOpening info = fetch_from_lichess(fen);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[fen] = info;
        }
        if (info.api_success && info.total_games <= 1) {
            unique_game_reached_ = true;
            break;
        }
        // Lichess rate limits to 1 per second generally; buffer to stay safe.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
}

} // namespace cta