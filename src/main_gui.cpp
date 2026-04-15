// Extracted from cpp directory
#include "MainWindow.h"
#include <QApplication>
#include <QMetaType>
#include <CLI/CLI.hpp>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <exception>
#include <stdlib.h>
#include <stdio.h>
#endif

static void set_ffmpeg_threads(int threads) {
    std::string val = std::to_string(threads);
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_THREADS", val.c_str());
#else
    setenv("OPENCV_FFMPEG_THREADS", val.c_str(), 1);
#endif
}

// Global exception handler for Windows SEH exceptions
#ifdef _WIN32
LONG WINAPI UnhandledExceptionFilter_(EXCEPTION_POINTERS* ExceptionInfo) {
    MessageBoxA(nullptr,
        "A fatal error occurred in the application.",
        "ChessTube Analyzer - Fatal Error",
        MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledExceptionFilter_);
#endif

    // Parse CLI arguments before QApplication
    CLI::App cliApp{"ChessTube Analyzer GUI — Process chess videos with optional headless mode"};

    std::string video_path;
    cliApp.add_option("video_path", video_path, "Path to the input video file (enables headless mode)")
        ->check(CLI::ExistingFile);

    std::string board_asset;
    cliApp.add_option("--board-asset", board_asset, "Path to board template image")
        ->check(CLI::ExistingFile);

    std::string output = "";
    cliApp.add_option("--output", output, "Path to save the extracted JSON data");

    bool generate_pgn = true;
    cliApp.add_flag("--pgn", generate_pgn, "Enable PGN file generation (default: on)");

    bool enable_stockfish = false;
    cliApp.add_flag("--stockfish", enable_stockfish, "Enable Stockfish engine analysis");

    int multi_pv = 0; // 0 means use saved/default
    cliApp.add_option("--multi-pv", multi_pv, "Number of best lines for Stockfish (1-4)")
        ->check(CLI::Range(1, 4));

    int ffmpeg_threads = 0; // 0 means use saved/default
    cliApp.add_option("--threads", ffmpeg_threads, "FFmpeg decode threads (1-16)")
        ->check(CLI::Range(1, 16));

    bool show_version = false;
    cliApp.add_flag("--version,-v", show_version, "Show version and exit");

    try {
        cliApp.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        std::cout << cliApp.help();
        return e.get_exit_code();
    }

    if (show_version) {
        std::cout << "ChessTube Analyzer v0.3.0" << std::endl;
        return 0;
    }

    // Register custom types for queued signal/slot connections
    qRegisterMetaType<aa::ProcessingSettings>("ProcessingSettings");

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    QApplication::setApplicationName("ChessTube Analyzer GUI");
    QApplication::setApplicationVersion("0.3.0");

    aa::MainWindow window;

    // Headless mode: if video_path is provided, process and exit
    if (!video_path.empty()) {
        window.showMinimized();
        int result = window.processHeadless(QString::fromStdString(video_path));
        return result;
    }

    window.show();
    return app.exec();
}
