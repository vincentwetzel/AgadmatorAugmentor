// Extracted from cpp directory
#include "ChessVideoExtractor.h"
#include "PgnWriter.h"
#include <CLI/CLI.hpp>
#include <iostream>
#include <fstream>

#include <string>
#include <thread>
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
    app.add_option("--output", output, "Path to save the extracted PGN data");

    std::string debug_level_str = "MOVES";
    app.add_option("--debug-level", debug_level_str, "Detail level for debug image generation")
        ->check(CLI::IsMember({"NONE", "MOVES", "FULL"}));

    int threads = 0;
    app.add_option("--threads", threads, "FFmpeg decode threads (1-16)")->check(CLI::Range(1, 16));

    int memory_limit = 0;
    app.add_option("--memory-limit", memory_limit, "Memory limit in MB (0 = Unlimited)")->check(CLI::Range(0, 65536));

    CLI11_PARSE(app, argc, argv);

    if (threads > 0) {
        set_ffmpeg_threads(threads);
    } else {
        set_ffmpeg_threads(std::thread::hardware_concurrency());
    }

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
        output = "output/" + base_name + ".pgn";
    }

    aa::DebugLevel debug_level = aa::DebugLevel::Moves;
    if (debug_level_str == "NONE") debug_level = aa::DebugLevel::None;
    else if (debug_level_str == "FULL") debug_level = aa::DebugLevel::Full;

    std::cout << "Starting C++ Extraction on: " << video_path << "\n\n";

    try {
        aa::ChessVideoExtractor extractor(board_asset, "", debug_level, memory_limit);
        aa::GameData data = extractor.extract_moves_from_video(video_path, "");

        aa::PgnWriter pgn;
        pgn.add_header("Event", "ChessTube Analysis");
        pgn.add_header("Site", "Unknown");
        pgn.add_header("Date", "Unknown");
        pgn.add_header("Round", "1");
        pgn.add_header("White", "Unknown");
        pgn.add_header("Black", "Unknown");

        for (size_t i = 0; i < data.moves.size(); ++i) {
            std::string clockStr = "0:00:00";
            size_t clockIdx = i + 1;
            
            if (clockIdx < data.clocks.size()) {
                clockStr = (i % 2 == 0) ? data.clocks[clockIdx].white_time : data.clocks[clockIdx].black_time;
            } else if (i < data.clocks.size()) {
                clockStr = (i % 2 == 0) ? data.clocks[i].white_time : data.clocks[i].black_time;
            }

            // Format basic h:mm:ss if it's missing colons
            if (clockStr.empty()) clockStr = "0:00:00";
            else if (std::count(clockStr.begin(), clockStr.end(), ':') == 0) {
                clockStr = "0:00:" + clockStr;
            } else if (std::count(clockStr.begin(), clockStr.end(), ':') == 1) {
                clockStr = "0:" + clockStr;
            }
            
            pgn.add_ply(data.moves[i], clockStr);
        }

        std::string pgnContent = pgn.build();
        std::ofstream pgnFile(output);
        if (pgnFile.is_open()) {
            pgnFile << pgnContent;
            pgnFile.close();
        } else {
            std::cerr << "Warning: Could not open output file for writing: " << output << "\n";
        }

        std::cout << "\nExtraction complete! " << data.moves.size() << " plies extracted.\n";
        std::cout << "PGN data saved to " << output << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
