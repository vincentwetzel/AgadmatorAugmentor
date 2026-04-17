// Extracted from cpp directory
#include "StockfishAnalyzer.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <array>
#include <thread>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <map>
#include <atomic>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace aa {

namespace { // Anonymous namespace for helper

static std::string get_stockfish_executable_path() {
#ifdef _WIN32
    std::string execName = "stockfish.exe";
#else
    std::string execName = "stockfish";
#endif
    std::vector<std::filesystem::path> paths_to_check = {
        "stockfish/" + execName,      // For running from project root
        "../stockfish/" + execName,   // For running from build/
        "../../stockfish/" + execName,// For running from build/Release or build/Debug
        execName
    };

    for (const auto& p : paths_to_check) {
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    
    // Fallback to the original path if none are found.
    return "stockfish/" + execName;
}

} // anonymous namespace

struct StockfishAnalyzer::StockfishImpl {
#ifdef _WIN32
    HANDLE hProcess = nullptr;
    HANDLE hChildStdinRead = nullptr;
    HANDLE hChildStdinWrite = nullptr;
    HANDLE hChildStdoutRead = nullptr;
    HANDLE hChildStdoutWrite = nullptr;
#else
    FILE* to_child = nullptr;
    FILE* from_child = nullptr;
#endif

    bool initialized = false;

    void initialize(const std::string& stockfish_path_arg) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        // Create pipe for child's STDOUT
        if (!CreatePipe(&hChildStdoutRead, &hChildStdoutWrite, &saAttr, 0))
            throw std::runtime_error("Failed to create stdout pipe");
        if (!SetHandleInformation(hChildStdoutRead, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("Failed to set stdout handle");

        // Create pipe for child's STDIN
        if (!CreatePipe(&hChildStdinRead, &hChildStdinWrite, &saAttr, 0))
            throw std::runtime_error("Failed to create stdin pipe");
        if (!SetHandleInformation(hChildStdinWrite, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("Failed to set stdin handle");

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.hStdInput = hChildStdinRead;
        si.hStdOutput = hChildStdoutWrite;
        si.hStdError = hChildStdoutWrite;
        si.dwFlags |= STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        std::string stockfish_path;
        if (!stockfish_path_arg.empty() && std::filesystem::exists(stockfish_path_arg)) {
            stockfish_path = stockfish_path_arg;
        } else {
            stockfish_path = get_stockfish_executable_path();
        }

        std::string cmd = "\"" + stockfish_path + "\"";
        if (!CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            throw std::runtime_error("Failed to start Stockfish process. Error: " + 
                                   std::to_string(GetLastError()) + ". Path: " + stockfish_path);
        }

        hProcess = pi.hProcess;
        CloseHandle(pi.hThread);

        // Close unused ends
        CloseHandle(hChildStdinRead);
        CloseHandle(hChildStdoutWrite);

        initialized = true; // Set to true before attempting to communicate

        // Initialize UCI
        send_command("uci");
        wait_for_response("uciok", nullptr);

        unsigned int hw_threads = std::thread::hardware_concurrency();
        if (hw_threads > 1) {
            send_command("setoption name Threads value " + std::to_string(hw_threads));
        }
        send_command("setoption name Hash value 256");
#else
        // POSIX implementation using popen
        to_child = nullptr;
        from_child = nullptr;
        throw std::runtime_error("POSIX implementation not yet complete");
#endif
    }

    void send_command(const std::string& cmd) {
#ifdef _WIN32
        if (!initialized || !hChildStdinWrite) return;
        DWORD written;
        std::string full_cmd = cmd + "\n";
        WriteFile(hChildStdinWrite, full_cmd.c_str(), (DWORD)full_cmd.size(), &written, NULL);
#endif
    }

    std::string read_response(int timeout_ms) {
#ifdef _WIN32
        if (!initialized || !hChildStdoutRead) return "";

        char buffer[4096];
        DWORD bytesRead;
        std::string result;
        DWORD bytesAvailable = 0;

        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeout_ms) {
            // Check if there's data in the pipe
            if (PeekNamedPipe(hChildStdoutRead, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
                if (ReadFile(hChildStdoutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    result += buffer;
                }
            } else {
                    if (!result.empty()) {
                        return result; // Return early if we read some data, so the caller can check it immediately
                    }
                // No data, sleep briefly to avoid busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        return result;
#endif
    }

    std::string wait_for_response(const std::string& marker, std::atomic<bool>* cancel_flag) {
        std::string response;
        auto start = std::chrono::steady_clock::now();
        const int timeout_seconds = 300; // 5-minute safety timeout for very deep analysis

        while (true) {
            if (cancel_flag && *cancel_flag) {
                send_command("stop");
                read_response(200); // Clear some buffer
                return ""; // Return empty to signal cancellation
            }

            std::string chunk = read_response(100); // Read in smaller chunks for faster cancellation polling
            if (!chunk.empty()) {
                size_t old_size = response.size();
                response += chunk;
                size_t search_start = old_size >= marker.size() ? old_size - marker.size() : 0;
                if (response.find(marker, search_start) != std::string::npos) {
                    return response;
                }
            }
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > timeout_seconds) {
                throw std::runtime_error("Stockfish timeout waiting for marker: " + marker);
            }
        }
    }

    ~StockfishImpl() {
#ifdef _WIN32
        if (hChildStdinWrite) CloseHandle(hChildStdinWrite);
        if (hChildStdoutRead) CloseHandle(hChildStdoutRead);
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
        }
#endif
    }
};

StockfishAnalyzer::StockfishAnalyzer(int multi_pv, const std::string& stockfish_path) : multi_pv_(multi_pv) {
    impl_ = new StockfishImpl();
    impl_->initialize(stockfish_path);
    set_multi_pv(multi_pv);
}

StockfishAnalyzer::~StockfishAnalyzer() {
    delete impl_;
}

void StockfishAnalyzer::set_progress_callback(ProgressCallback cb) {
    progress_callback_ = std::move(cb);
}

void StockfishAnalyzer::set_multi_pv(int multi_pv) {
    multi_pv_ = std::clamp(multi_pv, 1, 4);
    impl_->send_command("setoption name MultiPV value " + std::to_string(multi_pv_));
}

StockfishResult StockfishAnalyzer::analyze_position(const std::string& fen, int depth, std::atomic<bool>* cancel_flag) {
    StockfishResult result;
    result.fen = fen;

    // Set position
    impl_->send_command("position fen " + fen);
    
    if (cancel_flag && *cancel_flag) return result;

    // Start analysis
    impl_->send_command("go depth " + std::to_string(depth));
    
    // Read analysis output
    std::string response = impl_->wait_for_response("bestmove", cancel_flag);
    
    if (response.empty()) { // Indicates cancellation
        return result;
    }
    // Use a map to store the latest (highest depth) info for each multipv value.
    std::map<int, StockfishLine> latest_lines;

    std::istringstream iss(response);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("info", 0) == 0) { // More robust check for "info" at start
            StockfishLine line_data;
            int multipv = 0;
            
            std::istringstream line_stream(line);
            std::string token;
            while (line_stream >> token) {
                if (token == "multipv") {
                    line_stream >> multipv;
                } else if (token == "score") {
                    line_stream >> token; // "cp" or "mate"
                    if (token == "cp") {
                        line_stream >> line_data.centipawns;
                        line_data.is_mate = false;
                    } else if (token == "mate") {
                        line_stream >> line_data.mate_in;
                        line_data.is_mate = true;
                    }
                } else if (token == "pv") {
                    // The rest of the line is the PV (Principal Variation)
                    std::string pv_rest;
                    size_t pos = line.find(" pv ");
                    if (pos != std::string::npos) {
                        pv_rest = line.substr(pos + 4);
                        pv_rest.erase(0, pv_rest.find_first_not_of(" \t\n\r")); // Trim leading whitespace
                        line_data.pv_line = pv_rest;

                        // Extract first move from PV
                        std::istringstream pv_stream(pv_rest);
                        pv_stream >> line_data.move_uci;
                    }
                    break; // Done with this line
                }
            }
            
            if (multipv > 0 && !line_data.pv_line.empty()) {
                // This overwrites older (lower depth) info for the same multipv, which is what we want.
                latest_lines[multipv] = line_data;
            }
        }
    }

    // Transfer the latest lines from the map to the result vector
    result.lines.reserve(latest_lines.size());
    for (const auto& [multipv, line_data] : latest_lines) {
        result.lines.push_back(line_data);
    }

    return result;
}

std::vector<StockfishResult> StockfishAnalyzer::analyze_positions(const std::vector<std::string>& fens, int depth, std::atomic<bool>* cancel_flag) {
    std::vector<StockfishResult> results;
    results.reserve(fens.size());

    for (size_t i = 0; i < fens.size(); ++i) {
        if (cancel_flag && *cancel_flag) {
            break;
        }
        if (progress_callback_) {
            progress_callback_(static_cast<int>(i + 1), static_cast<int>(fens.size()));
        }
        results.push_back(analyze_position(fens[i], depth, cancel_flag));
    }

    return results;
}

} // namespace aa
