// Extracted from cpp directory
#include "MainWindow.h"
#include "TemplateManager.h"
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QMetaType>
#include <QSettings>
#include <cstdlib>
#include <iostream>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <exception>
#include <stdlib.h>
#include <stdio.h>
#endif

namespace {

#ifdef _WIN32
LONG WINAPI UnhandledExceptionFilter_(EXCEPTION_POINTERS* exception_info);
#endif

void configure_platform_exception_handler() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledExceptionFilter_);
#endif
}

void set_ffmpeg_threads(int threads) {
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

bool parse_int_option(const QCommandLineParser& parser,
                      const QString& option_name,
                      int minimum,
                      int maximum,
                      int& output,
                      std::ostream& err) {
    if (!parser.isSet(option_name)) {
        return true;
    }

    bool ok = false;
    const int parsed_value = parser.value(option_name).toInt(&ok);
    if (!ok || parsed_value < minimum || parsed_value > maximum) {
        err << "Invalid value for --" << option_name.toStdString()
            << ". Expected an integer in [" << minimum << ", " << maximum << "].\n";
        return false;
    }

    output = parsed_value;
    return true;
}

bool validate_existing_file(const QString& path,
                            const char* option_name,
                            std::ostream& err) {
    if (path.isEmpty()) {
        return true;
    }

    if (!QFileInfo::exists(path) || !QFileInfo(path).isFile()) {
        err << "Path provided to " << option_name << " does not exist or is not a file: "
            << path.toStdString() << "\n";
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char *argv[]) {
    configure_platform_exception_handler();

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName("ChessTubeAnalyzer");
    QApplication::setApplicationName("ChessTubeAnalyzer");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QApplication::setApplicationVersion("0.3.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("ChessTube Analyzer GUI - Process chess videos with optional headless mode");
    parser.addHelpOption();

    QCommandLineOption version_option(QStringList{"v", "version"}, "Show version and exit");
    QCommandLineOption board_asset_option("board-asset", "Path to board template image.", "path");
    QCommandLineOption output_option("output", "Path to save the extracted data (PGN/Video).", "path");
    QCommandLineOption debug_level_option("debug-level", "Debug image generation (NONE, MOVES, FULL).", "level");
    QCommandLineOption pgn_option("pgn", "Enable PGN file generation.");
    QCommandLineOption stockfish_option("stockfish", "Enable Stockfish engine analysis.");
    QCommandLineOption multi_pv_option("multi-pv", "Number of best lines for Stockfish (1-4).", "count");
    QCommandLineOption depth_option("depth", "Stockfish search depth (1-24).", "depth");
    QCommandLineOption time_option("time", "Stockfish max time per move in seconds (0 = no limit).", "s");
    QCommandLineOption nodes_option("nodes", "Stockfish max nodes per move (0 = no limit).", "count");
    QCommandLineOption analysis_depth_option("analysis-depth", "Stockfish analysis line depth (1-20).", "depth");
    QCommandLineOption threads_option("threads", "FFmpeg decode threads (1-16).", "count");
    QCommandLineOption memory_limit_option("memory-limit", "Memory limit in MB (0 = Unlimited).", "mb");

    parser.addOption(version_option);
    parser.addOption(board_asset_option);
    parser.addOption(output_option);
    parser.addOption(debug_level_option);
    parser.addOption(pgn_option);
    parser.addOption(stockfish_option);
    parser.addOption(multi_pv_option);
    parser.addOption(depth_option);
    parser.addOption(time_option);
    parser.addOption(nodes_option);
    parser.addOption(analysis_depth_option);
    parser.addOption(threads_option);
    parser.addOption(memory_limit_option);
    parser.addPositionalArgument("video_path", "Path to the input video file (enables headless mode).");

    if (!parser.parse(QCoreApplication::arguments())) {
        std::cerr << parser.errorText().toStdString() << "\n\n"
                  << parser.helpText().toStdString();
        return 1;
    }

    if (parser.isSet(version_option)) {
        std::cout << "ChessTube Analyzer v0.3.0" << std::endl;
        return 0;
    }

    // Initialize the template manager to load/copy templates from/to AppData.
    // This makes them available for both GUI and headless mode.
    cta::TemplateManager::instance().initialize();

    const QStringList positional_arguments = parser.positionalArguments();
    if (positional_arguments.size() > 1) {
        std::cerr << "Only one positional video_path is supported.\n\n"
                  << parser.helpText().toStdString();
        return 1;
    }

    const QString video_path = positional_arguments.isEmpty() ? QString{} : positional_arguments.front();
    const QString board_asset = parser.value(board_asset_option);
    const QString output = parser.value(output_option);
    const QString debug_level_str = parser.value(debug_level_option);

    int multi_pv = -1, depth = -1, time = -1, nodes = -1, analysis_depth = -1, threads = -1, memory_limit = -1;
    
    if (!parse_int_option(parser, "multi-pv", 1, 4, multi_pv, std::cerr) ||
        !parse_int_option(parser, "depth", 1, 40, depth, std::cerr) ||
        !parse_int_option(parser, "time", 0, 600, time, std::cerr) ||
        !parse_int_option(parser, "nodes", 0, 1000000000, nodes, std::cerr) ||
        !parse_int_option(parser, "analysis-depth", 1, 20, analysis_depth, std::cerr) ||
        !parse_int_option(parser, "threads", 1, 16, threads, std::cerr) ||
        !parse_int_option(parser, "memory-limit", 0, 65536, memory_limit, std::cerr)) {
        return 1;
    }

    if (!validate_existing_file(video_path, "video_path", std::cerr) ||
        !validate_existing_file(board_asset, "--board-asset", std::cerr)) {
        return 1;
    }

    int pgn_override = parser.isSet(pgn_option) ? 1 : -1;
    int stockfish_override = parser.isSet(stockfish_option) ? 1 : -1;

    cta::MainWindow main_window;
    
    if (!video_path.isEmpty()) {
        return main_window.processHeadless(video_path, pgn_override, stockfish_override, multi_pv, threads, depth, time, nodes, analysis_depth, debug_level_str, output, board_asset, memory_limit);
    }

    main_window.show();
    return app.exec();
}