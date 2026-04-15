// Extracted from cpp directory
#include "ChessVideoExtractor.h"
#include <CLI/CLI.hpp>
#include <iostream>

#include <string>
#ifdef _WIN32
#include <stdlib.h>
#endif

static void set_ffmpeg_threads(int threads) {
    std::string val = std::to_string(threads);
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_THREADS", val.c_str());
#else
    setenv("OPENCV_FFMPEG_THREADS", val.c_str(), 1);
#endif
}

int main(int argc, char* argv[]) {
    CLI::App app{"ChessTube Analyzer — Extract chess plies from video"};

    std::string video_path = "";
    app.add_option("video_path", video_path, "Path to the input video file");

    std::string board_asset = "assets/board/board.png";
    app.add_option("--board-asset", board_asset, "Path to board template image");

    std::string output = "";
    app.add_option("--output", output, "Path to save the extracted JSON data");

    std::string debug_level_str = "MOVES";
    app.add_option("--debug-level", debug_level_str, "Detail level for debug image generation")
        ->check(CLI::IsMember({"NONE", "MOVES", "FULL"}));

    CLI11_PARSE(app, argc, argv);

    // F5 convenience: use sample video when no args provided
    if (video_path.empty()) {
        video_path = R"(e:\coding_workspaces\CPP\ChessTubeAnalyzer\assets\sample_games_short\7 plies\7 plies.mp4)";
        board_asset = R"(e:\coding_workspaces\CPP\ChessTubeAnalyzer\assets\board\board.png)";
        debug_level_str = "MOVES";
    }

    if (output.empty()) {
        std::string base_name = "analysis";
        size_t slash = video_path.find_last_of("/\\");
        std::string name_only = (slash != std::string::npos) ? video_path.substr(slash + 1) : video_path;
        size_t dot = name_only.find_last_of('.');
        if (dot != std::string::npos) {
            base_name = name_only.substr(0, dot);
        } else if (!name_only.empty()) {
            base_name = name_only;
        }
        output = "output/" + base_name + ".json";
    }

    aa::DebugLevel debug_level = aa::DebugLevel::Moves;
    if (debug_level_str == "NONE") debug_level = aa::DebugLevel::None;
    else if (debug_level_str == "FULL") debug_level = aa::DebugLevel::Full;

    std::cout << "Starting C++ Extraction on: " << video_path << "\n\n";

    try {
        aa::ChessVideoExtractor extractor(board_asset, "", debug_level);
        aa::GameData data = extractor.extract_moves_from_video(video_path, output);

        std::cout << "\nExtraction complete! " << data.moves.size() << " plies extracted.\n";
        std::cout << "Game data saved to " << output << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
