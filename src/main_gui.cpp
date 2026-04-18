// Extracted from cpp directory
#include "MainWindow.h"
#include <QApplication>
#include <QMetaType>
#include <CLI/CLI.hpp>
#include <iostream>
#include <thread>

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
    cliApp.add_option("--output", output, "Path to save the extracted data (PGN/Video)");

    std::string debug_level_str = "";
    cliApp.add_option("--debug-level", debug_level_str, "Debug image generation (NONE, MOVES, FULL)")
        ->check(CLI::IsMember({"NONE", "MOVES", "FULL"}));

    bool generate_pgn = true;
    cliApp.add_flag("--pgn", generate_pgn, "Enable PGN file generation (default: on)");

    bool enable_stockfish = false;
    cliApp.add_flag("--stockfish", enable_stockfish, "Enable Stockfish engine analysis");

    int multi_pv = 0; // 0 means use saved/default
    cliApp.add_option("--multi-pv", multi_pv, "Number of best lines for Stockfish (1-4)")
        ->check(CLI::Range(1, 4));

    int stockfish_depth = 0;
    cliApp.add_option("--depth", stockfish_depth, "Stockfish search depth (1-24)")
        ->check(CLI::Range(1, 24));

    int stockfish_analysis_depth = 0;
    cliApp.add_option("--analysis-depth", stockfish_analysis_depth, "Stockfish analysis line depth (1-20)")
        ->check(CLI::Range(1, 20));

    int ffmpeg_threads = 0; // 0 means use saved/default
    cliApp.add_option("--threads", ffmpeg_threads, "FFmpeg decode threads (1-16)")
        ->check(CLI::Range(1, 16));

    int memory_limit = -1; // -1 means use saved/default
    cliApp.add_option("--memory-limit", memory_limit, "Memory Limit in MB (0 = Unlimited)")
        ->check(CLI::Range(0, 65536));

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

    if (ffmpeg_threads > 0) {
        set_ffmpeg_threads(ffmpeg_threads);
    } else {
        set_ffmpeg_threads(std::thread::hardware_concurrency());
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
        
        int pgn_override = cliApp.get_option("--pgn")->count() > 0 ? (generate_pgn ? 1 : 0) : -1;
        int stockfish_override = cliApp.get_option("--stockfish")->count() > 0 ? (enable_stockfish ? 1 : 0) : -1;

        int result = window.processHeadless(QString::fromStdString(video_path), 
                                            pgn_override, 
                                            stockfish_override, 
                                            multi_pv, 
                                            ffmpeg_threads, 
                                            stockfish_depth, 
                                            stockfish_analysis_depth,
                                            QString::fromStdString(debug_level_str),
                                            QString::fromStdString(output),
                                            QString::fromStdString(board_asset),
                                            memory_limit);
        return result;
    }

    window.show();
    return app.exec();
}
