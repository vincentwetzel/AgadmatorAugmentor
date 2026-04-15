// Extracted from cpp directory
#include "StockfishAnalyzer.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <array>
#include <thread>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace aa {

// Path to Stockfish binary - configurable
static const std::string STOCKFISH_PATH = "stockfish/stockfish.exe";

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

    void initialize() {
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

        std::string cmd = "\"" + STOCKFISH_PATH + "\"";
        if (!CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            throw std::runtime_error("Failed to start Stockfish process. Error: " + 
                                   std::to_string(GetLastError()));
        }

        hProcess = pi.hProcess;
        CloseHandle(pi.hThread);

        // Close unused ends
        CloseHandle(hChildStdinRead);
        CloseHandle(hChildStdoutWrite);

        // Initialize UCI
        send_command("uci");
        wait_for_response("uciok");

        initialized = true;
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

    std::string read_response(int timeout_ms = 5000) {
#ifdef _WIN32
        if (!initialized || !hChildStdoutRead) return "";
        
        char buffer[4096];
        DWORD bytesRead;
        std::string result;
        
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (PeekNamedPipe(hChildStdoutRead, NULL, 0, NULL, NULL, NULL)) {
                if (ReadFile(hChildStdoutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                    buffer[bytesRead] = '\0';
                    result += buffer;
                    if (bytesRead < sizeof(buffer) - 1) break;
                }
            }
            
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return result;
#endif
    }

    void wait_for_response(const std::string& marker) {
        std::string response;
        while (true) {
            std::string chunk = read_response();
            response += chunk;
            if (response.find(marker) != std::string::npos) {
                break;
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

StockfishAnalyzer::StockfishAnalyzer(int multi_pv) : multi_pv_(multi_pv) {
    impl_ = new StockfishImpl();
    impl_->initialize();
    set_multi_pv(multi_pv);
}

StockfishAnalyzer::~StockfishAnalyzer() {
    delete impl_;
}

void StockfishAnalyzer::set_multi_pv(int multi_pv) {
    multi_pv_ = std::clamp(multi_pv, 1, 4);
    impl_->send_command("setoption name MultiPV value " + std::to_string(multi_pv_));
}

StockfishResult StockfishAnalyzer::analyze_position(const std::string& fen) {
    StockfishResult result;
    result.fen = fen;
    result.lines.reserve(multi_pv_);

    // Set position
    impl_->send_command("position fen " + fen);
    
    // Start analysis
    impl_->send_command("go depth 15");  // Depth 15 for reasonable speed
    
    // Read analysis output
    std::string response = impl_->read_response(10000);  // 10 second timeout
    
    // Parse bestmove line
    // Expected format: bestmove e2e4 ponder e7e5
    std::istringstream iss(response);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("info depth") != std::string::npos && 
            line.find("multipv") != std::string::npos) {
            // Parse info line
            // Example: info depth 15 seldepth 20 multipv 1 score cp 42 nodes 12345 pv e2e4 e7e5
            StockfishLine line_data;
            
            std::istringstream line_stream(line);
            std::string token;
            while (line_stream >> token) {
                if (token == "multipv") {
                    line_stream >> token;  // skip to value
                    // We'll add it based on order
                } else if (token == "score") {
                    line_stream >> token;
                    if (token == "cp") {
                        line_stream >> line_data.centipawns;
                        line_data.is_mate = false;
                    } else if (token == "mate") {
                        line_stream >> line_data.mate_in;
                        line_data.is_mate = true;
                    }
                } else if (token == "pv") {
                    // Rest of line is PV
                    std::string pv_rest;
                    std::getline(line_stream, pv_rest);
                    line_data.pv_line = pv_rest;
                    
                    // Extract first move from PV
                    std::istringstream pv_stream(pv_rest);
                    pv_stream >> line_data.move_uci;
                    break;
                }
            }
            
            result.lines.push_back(line_data);
        } else if (line.find("bestmove") != std::string::npos) {
            break;  // Analysis complete
        }
    }

    return result;
}

std::vector<StockfishResult> StockfishAnalyzer::analyze_positions(const std::vector<std::string>& fens) {
    std::vector<StockfishResult> results;
    results.reserve(fens.size());

    for (size_t i = 0; i < fens.size(); ++i) {
        results.push_back(analyze_position(fens[i]));
    }

    return results;
}

} // namespace aa
