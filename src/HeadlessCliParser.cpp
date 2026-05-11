#include "HeadlessCliParser.h"
#include "SysUtils.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFileInfo>

namespace cta {

namespace {
bool parse_int_option(const QCommandLineParser& parser, const QString& option_name, int minimum, int maximum, int& output, QString& err) {
    if (!parser.isSet(option_name)) return true;
    bool ok = false;
    const int parsed_value = parser.value(option_name).toInt(&ok);
    if (!ok || parsed_value < minimum || parsed_value > maximum) {
        err = QString("Invalid value for --%1. Expected an integer in [%2, %3].\n").arg(option_name).arg(minimum).arg(maximum);
        return false;
    }
    output = parsed_value;
    return true;
}

bool validate_existing_file(const QString& path, const char* option_name, QString& err) {
    if (path.isEmpty()) return true;
    if (!QFileInfo::exists(path) || !QFileInfo(path).isFile()) {
        err = QString("Path provided to %1 does not exist or is not a file: %2\n").arg(option_name, path);
        return false;
    }
    return true;
}

bool validate_video_file(const QString& path, QString& err) {
    if (path.isEmpty()) return true;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != "mp4" && suffix != "mkv" && suffix != "avi" && suffix != "mov" && suffix != "webm") {
        err = QString("Positional argument must be a video file (*.mp4, *.mkv, *.avi, *.mov, *.webm): %1\n").arg(path);
        return false;
    }
    return true;
}
} // namespace

CliOptions HeadlessCliParser::parse() {
    CliOptions opts;
    QCommandLineParser parser;
    parser.setApplicationDescription("ChessTube Analyzer GUI - Process chess videos with optional headless mode");
    parser.addHelpOption();

    QCommandLineOption version_option(QStringList{"v", "version"}, "Show version and exit");
    QCommandLineOption board_asset_option("board-asset", "Path to board template image.", "path");
    QCommandLineOption output_option("output", "Path to save the extracted data (PGN/Video).", "path");
    QCommandLineOption debug_level_option("debug-level", "Debug image generation (NONE, MOVES, FULL).", "level");
    QCommandLineOption pgn_option("pgn", "Enable PGN file generation.");
    QCommandLineOption analysis_video_option("analysis-video", "Enable analysis video generation.");
    QCommandLineOption move_labels_option("move-labels", "Enable move quality labels (runs Stockfish).");
    QCommandLineOption multi_pv_option("multi-pv", "Number of best lines for Stockfish (1-4).", "count");
    QCommandLineOption depth_option("depth", "Stockfish search depth (1-24).", "depth");
    QCommandLineOption time_option("time", "Stockfish max time per move in seconds (0 = no limit).", "s");
    QCommandLineOption nodes_option("nodes", "Stockfish max nodes per move (0 = no limit).", "count");
    QCommandLineOption analysis_depth_option("analysis-depth", "Stockfish analysis line depth (1-20).", "depth");
    const int max_threads = cta::SysUtils::max_hardware_thread_count();
    QCommandLineOption threads_option("threads", QString("FFmpeg decode threads (1-%1).").arg(max_threads), "count");
    QCommandLineOption memory_limit_option("memory-limit", "Memory limit in MB (0 = Unlimited).", "mb");

    parser.addOption(version_option);
    parser.addOption(board_asset_option);
    parser.addOption(output_option);
    parser.addOption(debug_level_option);
    parser.addOption(pgn_option);
    parser.addOption(analysis_video_option);
    parser.addOption(move_labels_option);
    parser.addOption(multi_pv_option);
    parser.addOption(depth_option);
    parser.addOption(time_option);
    parser.addOption(nodes_option);
    parser.addOption(analysis_depth_option);
    parser.addOption(threads_option);
    parser.addOption(memory_limit_option);
    parser.addPositionalArgument("video_path", "Path to the input video file (enables headless mode).");

    if (!parser.parse(QCoreApplication::arguments())) {
        opts.parseError = true;
        opts.errorMessage = parser.errorText();
        opts.helpText = parser.helpText();
        return opts;
    }

    if (parser.isSet(version_option)) {
        opts.showVersion = true;
        return opts;
    }

    const QStringList positional_arguments = parser.positionalArguments();
    if (positional_arguments.size() > 1) {
        opts.parseError = true;
        opts.errorMessage = "Only one positional video_path is supported.\n";
        opts.helpText = parser.helpText();
        return opts;
    }

    opts.videoPath = positional_arguments.isEmpty() ? QString{} : positional_arguments.front();
    opts.isHeadless = !opts.videoPath.isEmpty();
    opts.boardAsset = parser.value(board_asset_option);
    opts.output = parser.value(output_option);
    opts.debugLevelStr = parser.value(debug_level_option);
    opts.pgnOverride = parser.isSet(pgn_option) ? 1 : -1;
    opts.analysisVideoOverride = parser.isSet(analysis_video_option) ? 1 : -1;
    opts.moveLabelsOverride = parser.isSet(move_labels_option) ? 1 : -1;

    if (!parse_int_option(parser, "multi-pv", 1, 4, opts.multiPv, opts.errorMessage) || !parse_int_option(parser, "depth", 1, 40, opts.depth, opts.errorMessage) || !parse_int_option(parser, "time", 0, 600, opts.time, opts.errorMessage) || !parse_int_option(parser, "nodes", 0, 1000000000, opts.nodes, opts.errorMessage) || !parse_int_option(parser, "analysis-depth", 1, 20, opts.analysisDepth, opts.errorMessage) || !parse_int_option(parser, "threads", 1, max_threads, opts.threads, opts.errorMessage) || !parse_int_option(parser, "memory-limit", 0, 65536, opts.memoryLimit, opts.errorMessage) || !validate_existing_file(opts.videoPath, "video_path", opts.errorMessage) || !validate_video_file(opts.videoPath, opts.errorMessage) || !validate_existing_file(opts.boardAsset, "--board-asset", opts.errorMessage)) { opts.parseError = true; opts.helpText = parser.helpText(); return opts; }

    return opts;
}

} // namespace cta