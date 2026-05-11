#include "FfmpegProcessRunner.h"
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <stdio.h>
#endif

namespace cta {

namespace {
#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (size <= 0) return std::wstring(text.begin(), text.end());
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string format_windows_error(DWORD error_code) {
    LPSTR message_buffer = nullptr;
    DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                reinterpret_cast<LPSTR>(&message_buffer), 0, nullptr);
    std::string message = size > 0 && message_buffer ? message_buffer : "Unknown Windows error";
    if (message_buffer) LocalFree(message_buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) message.pop_back();
    return message;
}
#endif

void append_tail(std::string& tail, const char* data, size_t size) {
    constexpr size_t kMaxTailBytes = 4096;
    tail.append(data, size);
    if (tail.size() > kMaxTailBytes) {
        tail.erase(0, tail.size() - kMaxTailBytes);
    }
}
} // namespace

int FfmpegProcessRunner::run_with_progress(const std::string& cmd, int total_frames, std::atomic<bool>* cancel_flag, std::function<void(int, const std::string&)> progress_callback, std::string& out_tail) {
    int result = -1;
#ifdef _WIN32
    SECURITY_ATTRIBUTES saAttr; 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
        if (progress_callback) progress_callback(-1, "Failed to create pipes for FFmpeg.");
        return -1;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    std::wstring cmd_w = utf8_to_wide(cmd);
    std::vector<wchar_t> cmd_buffer(cmd_w.begin(), cmd_w.end());
    cmd_buffer.push_back(L'\0');

    if (CreateProcessW(NULL, cmd_buffer.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe);
        
        char buffer[256];
        DWORD bytesRead;
        std::string output_acc;
        
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            if (cancel_flag && *cancel_flag) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            buffer[bytesRead] = '\0';
            append_tail(out_tail, buffer, bytesRead);
            output_acc += buffer;
            
            size_t frame_pos = output_acc.rfind("frame=");
            if (frame_pos != std::string::npos) {
                size_t end_pos = output_acc.find("fps=", frame_pos);
                if (end_pos != std::string::npos) {
                    std::string frame_str = output_acc.substr(frame_pos + 6, end_pos - (frame_pos + 6));
                    try {
                        int frame_num = std::stoi(frame_str);
                        if (total_frames > 0 && progress_callback) {
                            int percent = 80 + (frame_num * 20) / total_frames;
                            percent = std::clamp(percent, 80, 99);
                            progress_callback(percent, "Muxing video: frame " + std::to_string(frame_num) + " / " + std::to_string(total_frames));
                        }
                    } catch (...) {}
                    output_acc.erase(0, end_pos);
                }
            }
            if (output_acc.length() > 4096) {
                output_acc.erase(0, output_acc.length() - 2048);
            }
        }
        
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            result = static_cast<int>(exit_code);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);
    } else {
        out_tail = "CreateProcessW failed: " + format_windows_error(GetLastError());
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
    }
#else
    std::string full_cmd = cmd + " 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (pipe) {
        char buffer[256];
        std::string output_acc;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            if (cancel_flag && *cancel_flag) break;
            append_tail(out_tail, buffer, std::strlen(buffer));
            output_acc += buffer;
            size_t frame_pos = output_acc.rfind("frame=");
            if (frame_pos != std::string::npos) {
                size_t end_pos = output_acc.find("fps=", frame_pos);
                if (end_pos != std::string::npos) {
                    std::string frame_str = output_acc.substr(frame_pos + 6, end_pos - (frame_pos + 6));
                    try {
                        int frame_num = std::stoi(frame_str);
                        if (total_frames > 0 && progress_callback) {
                            int percent = 80 + (frame_num * 20) / total_frames;
                            percent = std::clamp(percent, 80, 99);
                            progress_callback(percent, "Muxing video: frame " + std::to_string(frame_num) + " / " + std::to_string(total_frames));
                        }
                    } catch (...) {}
                    output_acc.erase(0, end_pos);
                }
            }
            if (output_acc.length() > 4096) {
                output_acc.erase(0, output_acc.length() - 2048);
            }
        }
        result = pclose(pipe);
    }
#endif
    return result;
}

} // namespace cta