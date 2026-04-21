// Extracted from cpp directory
#include "VideoProcessorWorker.h"
#include "ChessVideoExtractor.h"
#include "AnalysisVideoGenerator.h"
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"
#include "BoardLocalizer.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"

#include <QSettings>
#include <QCoreApplication>
#include <exception>
#include <fstream>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <map>
#include <sstream>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aa {

namespace { // Anonymous namespace for helper function

    std::array<char, 64> expand_fen_to_board(const std::string& fen) {
        std::array<char, 64> board;
        board.fill(' ');
        int sq = 56;
        for (char c : fen) {
            if (c == ' ') break;
            if (c == '/') sq -= 16;
            else if (c >= '1' && c <= '8') sq += (c - '0');
            else board[sq++] = c;
        }
        return board;
    }

    std::string build_san(const libchess::Position& pos, const libchess::Move& move, const std::string& uci_str) {
        auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
        auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));
        std::array<char, 64> board = expand_fen_to_board(pos.get_fen());
        char piece = board[from_sq];
        char target_piece = board[to_sq];
        bool is_pawn = (piece == 'P' || piece == 'p');
        bool is_capture = (target_piece != ' ') || (is_pawn && (from_sq % 8) != (to_sq % 8) && target_piece == ' ');
        
        if (move.type() == libchess::MoveType::ksc) return "O-O";
        if (move.type() == libchess::MoveType::qsc) return "O-O-O";
        if ((piece == 'K' || piece == 'k') && std::abs((from_sq % 8) - (to_sq % 8)) == 2) {
            if (to_sq % 8 == 6) return "O-O";
            if (to_sq % 8 == 2) return "O-O-O";
        }
        std::string san;
        if (!is_pawn) {
            san += static_cast<char>(std::toupper(piece));
            bool file_conflict = false, rank_conflict = false, need_disambiguation = false;
            for (const auto& alt_move : pos.legal_moves()) {
                auto alt_from = static_cast<int>(static_cast<unsigned int>(alt_move.from()));
                auto alt_to = static_cast<int>(static_cast<unsigned int>(alt_move.to()));
                if (alt_from != from_sq && alt_to == to_sq && board[alt_from] == piece) {
                    need_disambiguation = true;
                    if (alt_from % 8 == from_sq % 8) file_conflict = true;
                    if (alt_from / 8 == from_sq / 8) rank_conflict = true;
                }
            }
            if (need_disambiguation) {
                if (!file_conflict) san += static_cast<char>('a' + (from_sq % 8));
                else if (!rank_conflict) san += static_cast<char>('1' + (from_sq / 8));
                else { san += static_cast<char>('a' + (from_sq % 8)); san += static_cast<char>('1' + (from_sq / 8)); }
            }
        } else {
            if (is_capture) san += static_cast<char>('a' + (from_sq % 8));
        }
        if (is_capture) san += "x";
        san += static_cast<char>('a' + (to_sq % 8));
        san += static_cast<char>('1' + (to_sq / 8));
        if (uci_str.length() >= 5) {
            san += "="; san += static_cast<char>(std::toupper(uci_str[4]));
        }
        libchess::Position temp_pos = pos;
        temp_pos.makemove(move);
        if (temp_pos.is_checkmate()) san += "#";
        else if (temp_pos.in_check()) san += "+";
        return san;
    }

    std::string format_clock_string(std::string clockStr) {
        // Standard PGN clock format requires hours: [%clk h:mm:ss]
        if (clockStr.empty()) {
            return "0:00:00"; // Fallback for blank or malformed clocks
        } else if (std::count(clockStr.begin(), clockStr.end(), ':') == 0) {
            // If there are no colons but there is a decimal (e.g. "14.5")
            size_t dot_pos = clockStr.find('.');
            if (dot_pos != std::string::npos) {
                if (dot_pos == 1) {
                    return "0:00:0" + clockStr; // e.g., "9.5" -> "0:00:09.5"
                } else {
                    return "0:00:" + clockStr;  // e.g., "14.5" -> "0:00:14.5"
                }
            } else {
                if (clockStr.length() == 1) {
                    return "0:00:0" + clockStr; // e.g., "9" -> "0:00:09"
                } else {
                    return "0:00:" + clockStr;  // e.g., "45" -> "0:00:45"
                }
            }
        } else if (std::count(clockStr.begin(), clockStr.end(), ':') == 1) {
            if (clockStr.find(':') == 1) {
                return "0:0" + clockStr; // e.g., 9:58 -> 0:09:58
            } else {
                return "0:" + clockStr;  // e.g., 10:00 -> 0:10:00
            }
        }
        return clockStr; // Already in h:mm:ss format
    }

    bool is_ffmpeg_available() {
#ifdef _WIN32
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        std::string cmd_str = "ffmpeg -version";
        std::vector<char> cmd(cmd_str.begin(), cmd_str.end());
        cmd.push_back('\0');

        if (CreateProcessA(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return exitCode == 0;
        }
        return false;
#else
        return std::system("ffmpeg -version > /dev/null 2>&1") == 0;
#endif
    }
}

void VideoProcessorWorker::process(const ProcessingSettings& settings, std::atomic<bool>* cancelFlag) {
    try {
        if (settings.generateAnalysisVideo) {
            emit logMessage("Verifying FFmpeg installation...");
            if (!is_ffmpeg_available()) {
                throw std::runtime_error("FFmpeg is required for Analysis Video generation but was not found in the system PATH. Please install FFmpeg and restart the application.");
            }
        }

        emit logMessage("Initializing extractor for: " + settings.videoPath);

        ChessVideoExtractor extractor(
            settings.boardAssetPath.toStdString(),
            "", // Red board template GUI option removed, backend will use fallback
            static_cast<DebugLevel>(settings.debugLevel),
            settings.memoryLimitMB
        );

        extractor.set_progress_callback([this](int percent, const std::string& message) {
            // The callback itself doesn't need to know about cancellation,
            // but this is where we could check if we wanted to cancel
            // during the callback logic itself. For now, the main loops
            // in the extractor will handle it.
            if (percent >= 0) {
                emit progressUpdated(percent);
            }
            if (!message.empty()) {
                emit logMessage(QString::fromStdString(message));
            }
        });

        // Step 1: Extract moves from video
        GameData gameData = extractor.extract_moves_from_video(
            settings.videoPath.toStdString(),
            "", // debug_label
            cancelFlag
        );

        if (cancelFlag && *cancelFlag) {
            emit finished();
            return;
        }

        // Step 2: Optional Stockfish analysis (decoupled from PGN generation)
        std::vector<StockfishResult> mainLineStockfishResults;
        std::vector<StockfishResult> videoStockfishResults;
        std::map<std::string, StockfishResult> fenAnalysisCache;

        if ((settings.enableStockfish || settings.generateAnalysisVideo) && !gameData.fens.empty()) {
            // 1. Gather all unique FENs from the video timeline
            std::vector<std::string> unique_fens;
            for (const auto& fen : gameData.video_fens) {
                if (std::find(unique_fens.begin(), unique_fens.end(), fen) == unique_fens.end()) {
                    unique_fens.push_back(fen);
                }
            }
            
            emit logMessage("Starting Stockfish analysis (MultiPV=" + QString::number(settings.multiPv) + ", " + QString::number(unique_fens.size()) + " unique positions)...");

            StockfishAnalyzer analyzer(settings.multiPv, settings.stockfishPath.toStdString());
            analyzer.set_progress_callback([this, cancelFlag](int current, int total) {
                QString msg = QString("Analyzing position %1 of %2...").arg(current).arg(total);
                emit logMessage(msg);
            });
            std::vector<StockfishResult> unique_results = analyzer.analyze_positions(unique_fens, settings.stockfishDepth, cancelFlag);
            if (cancelFlag && *cancelFlag) {
                emit finished();
                return;
            }
            
            // Truncate PV lines
            for (auto& result : unique_results) {
                for (auto& line : result.lines) {
                    std::istringstream pv_stream(line.pv_line);
                    std::string move_uci;
                    std::string truncated_pv;
                    int count = 0;
                    while (count < settings.stockfishAnalysisDepth && (pv_stream >> move_uci)) {
                        if (!truncated_pv.empty()) truncated_pv += " ";
                        truncated_pv += move_uci;
                        count++;
                    }
                    line.pv_line = truncated_pv;
                }
                fenAnalysisCache[result.fen] = result;
            }
            
            // Build synchronized results arrays
            for (const auto& fen : gameData.fens) {
                mainLineStockfishResults.push_back(fenAnalysisCache[fen]);
            }
            for (const auto& fen : gameData.video_fens) {
                videoStockfishResults.push_back(fenAnalysisCache[fen]);
            }
            
            emit logMessage("Stockfish analysis complete.");
        }

        // Step 2a: Optional move quality annotation based on Stockfish analysis
        std::vector<std::string> move_annotations(gameData.moves.size(), "");
            QSettings q_settings;
        bool enableMoveAnnotations = q_settings.value("analysis/enableMoveAnnotations", true).toBool();

        if (enableMoveAnnotations && (settings.enableStockfish || settings.generateAnalysisVideo)) {
            emit logMessage("Generating move quality annotations...");
            
            auto calculate_material = [](const std::string& fen) {
                int material = 0;
                for (char c : fen) {
                    if (c == ' ') break;
                    switch (c) {
                        case 'Q': material += 9; break; case 'q': material -= 9; break;
                        case 'R': material += 5; break; case 'r': material -= 5; break;
                        case 'B': material += 3; break; case 'b': material -= 3; break;
                        case 'N': material += 3; break; case 'n': material -= 3; break;
                        case 'P': material += 1; break; case 'p': material -= 1; break;
                    }
                }
                return material;
            };

            // Annotate Video Moves (for the video overlay)
            if (!videoStockfishResults.empty() && videoStockfishResults.size() > gameData.video_moves.size()) {
                for (size_t i = 0; i < gameData.video_moves.size(); ++i) {
                    if (gameData.video_moves[i] == "REVERT") continue;
                    
                    const auto& fen_before = gameData.video_fens[i];
                    bool is_white_to_move = (fen_before.find(" w ") != std::string::npos);
                    const auto& analysis_before = videoStockfishResults[i];
                    const auto& analysis_after = videoStockfishResults[i + 1];

                    if (analysis_before.lines.empty() || analysis_after.lines.empty()) continue;

                    int best_eval_white_pov;
                    if (analysis_before.lines[0].is_mate) {
                        int mate_in = analysis_before.lines[0].mate_in;
                        best_eval_white_pov = is_white_to_move ? ((mate_in > 0) ? 30000 : -30000) : ((mate_in > 0) ? -30000 : 30000);
                    } else {
                        best_eval_white_pov = is_white_to_move ? analysis_before.lines[0].centipawns : -analysis_before.lines[0].centipawns;
                    }

                    int played_eval_white_pov;
                    if (analysis_after.lines[0].is_mate) {
                        int mate_in = analysis_after.lines[0].mate_in;
                        played_eval_white_pov = !is_white_to_move ? ((mate_in > 0) ? -30000 : 30000) : ((mate_in > 0) ? 30000 : -30000);
                    } else {
                        played_eval_white_pov = !is_white_to_move ? analysis_after.lines[0].centipawns : -analysis_after.lines[0].centipawns;
                    }

                    int cp_loss = is_white_to_move ? (best_eval_white_pov - played_eval_white_pov) : (played_eval_white_pov - best_eval_white_pov);
                    if (cp_loss < 0) cp_loss = 0;

                    std::string video_ann = "";
                    int full_move = 1;
                    size_t last_space = fen_before.find_last_of(' ');
                    if (last_space != std::string::npos) {
                        try { full_move = std::stoi(fen_before.substr(last_space + 1)); } catch (...) {}
                    }
                    int ply_count = (full_move - 1) * 2 + (is_white_to_move ? 0 : 1);

                    if (ply_count < 10 && cp_loss <= 25) {
                        video_ann = " (Book)";
                    } else if (gameData.video_moves[i] == analysis_before.lines[0].move_uci) {
                        if (analysis_before.lines.size() > 1) {
                            int material_before = calculate_material(fen_before);
                            int material_after = calculate_material(gameData.video_fens[i+1]);
                            int material_diff = material_after - material_before;
                            bool is_sacrifice = (is_white_to_move && material_diff < 0) || (!is_white_to_move && material_diff > 0);
                            bool is_good_eval = is_white_to_move ? (best_eval_white_pov > -200) : (best_eval_white_pov < 200);

                            int diff_to_second = std::abs(analysis_before.lines[0].centipawns - analysis_before.lines[1].centipawns);
                            if (is_sacrifice && is_good_eval) {
                                video_ann = (diff_to_second > 200 && cp_loss <= 10) ? "!!" : "!";
                            } else {
                                video_ann = "*";
                            }
                        } else {
                            video_ann = "*";
                        }
                    } else if (cp_loss <= 25) {
                        video_ann = " (Good)";
                    } else if (cp_loss >= 300) {
                        video_ann = "??";
                    } else if (cp_loss >= 150) {
                        int side_eval_before = is_white_to_move ? best_eval_white_pov : -best_eval_white_pov;
                        video_ann = (side_eval_before >= 200) ? "X" : "?";
                    } else if (cp_loss >= 75) {
                        video_ann = "?";
                    }

                    aa::StockfishLine dummy;
                    dummy.move_uci = "ANNOTATION";
                    dummy.pv_line = gameData.video_moves[i] + video_ann;
                    videoStockfishResults[i + 1].lines.push_back(dummy);
                }
            }

            // Annotate Main Line Moves (for PGN)
            if (!mainLineStockfishResults.empty() && mainLineStockfishResults.size() > gameData.moves.size()) {
                for (size_t i = 0; i < gameData.moves.size(); ++i) {
                    const auto& fen_before = gameData.fens[i];
                    bool is_white_to_move = (fen_before.find(" w ") != std::string::npos);
                    const auto& analysis_before = mainLineStockfishResults[i];
                    const auto& analysis_after = mainLineStockfishResults[i + 1];

                    if (analysis_before.lines.empty() || analysis_after.lines.empty()) continue;

                    int best_eval_white_pov;
                    if (analysis_before.lines[0].is_mate) {
                        int mate_in = analysis_before.lines[0].mate_in;
                        best_eval_white_pov = is_white_to_move ? ((mate_in > 0) ? 30000 : -30000) : ((mate_in > 0) ? -30000 : 30000);
                    } else {
                        best_eval_white_pov = is_white_to_move ? analysis_before.lines[0].centipawns : -analysis_before.lines[0].centipawns;
                    }

                    int played_eval_white_pov;
                    if (analysis_after.lines[0].is_mate) {
                        int mate_in = analysis_after.lines[0].mate_in;
                        played_eval_white_pov = !is_white_to_move ? ((mate_in > 0) ? -30000 : 30000) : ((mate_in > 0) ? 30000 : -30000);
                    } else {
                        played_eval_white_pov = !is_white_to_move ? analysis_after.lines[0].centipawns : -analysis_after.lines[0].centipawns;
                    }

                    int cp_loss = is_white_to_move ? (best_eval_white_pov - played_eval_white_pov) : (played_eval_white_pov - best_eval_white_pov);
                    if (cp_loss < 0) cp_loss = 0;

                    std::string annotation = "";
                    int full_move = 1;
                    size_t last_space = fen_before.find_last_of(' ');
                    if (last_space != std::string::npos) {
                        try { full_move = std::stoi(fen_before.substr(last_space + 1)); } catch (...) {}
                    }
                    int ply_count = (full_move - 1) * 2 + (is_white_to_move ? 0 : 1);

                    if (ply_count < 10 && cp_loss <= 25) {
                        annotation = " \xF0\x9F\x93\x96"; // Book
                    } else if (gameData.moves[i] == analysis_before.lines[0].move_uci) {
                        if (analysis_before.lines.size() > 1) {
                            int material_before = calculate_material(fen_before);
                            int material_after = calculate_material(gameData.fens[i+1]);
                            int material_diff = material_after - material_before;
                            bool is_sacrifice = (is_white_to_move && material_diff < 0) || (!is_white_to_move && material_diff > 0);
                            bool is_good_eval = is_white_to_move ? (best_eval_white_pov > -200) : (best_eval_white_pov < 200);

                            int diff_to_second = std::abs(analysis_before.lines[0].centipawns - analysis_before.lines[1].centipawns);
                            if (is_sacrifice && is_good_eval) {
                                annotation = (diff_to_second > 200 && cp_loss <= 10) ? "!!" : "!";
                            } else {
                                annotation = "*";
                            }
                        } else {
                            annotation = "*";
                        }
                    } else if (cp_loss <= 25) {
                        annotation = "";
                    } else if (cp_loss >= 300) {
                        annotation = "??";
                    } else if (cp_loss >= 150) {
                        int side_eval_before = is_white_to_move ? best_eval_white_pov : -best_eval_white_pov;
                        annotation = (side_eval_before >= 200) ? "X" : "?";
                    } else if (cp_loss >= 75) {
                        annotation = "?";
                    }
                    
                    move_annotations[i] = annotation;
                }
            }
        }

        // Step 3: Optional PGN export
        if (settings.generatePgn) {
            emit logMessage("Generating PGN file...");
            PgnWriter pgn;

            // Add standard headers
            pgn.add_header("Event", "Unknown");
            pgn.add_header("Site", "Unknown");
            pgn.add_header("Date", "Unknown");
            pgn.add_header("Round", "Unknown");
            pgn.add_header("White", "Unknown");
            pgn.add_header("Black", "Unknown");

            // Helper to format eval comment for variations
            auto get_eval_str = [](const StockfishResult& res) -> std::string {
                if (res.lines.empty()) return "";
                const auto& best = res.lines[0];
                bool is_black_to_move = (res.fen.find(" b ") != std::string::npos);
                if (best.is_mate) {
                    int mate_in = best.mate_in;
                    if (is_black_to_move) mate_in = -mate_in;
                    return (mate_in > 0 ? "+M" : "-M") + std::to_string(std::abs(mate_in));
                }
                double eval_cp = best.centipawns / 100.0;
                if (is_black_to_move) eval_cp = -eval_cp;
                char buf[32];
                snprintf(buf, sizeof(buf), "%+.2f", eval_cp);
                return std::string(buf);
            };

            // Add moves with clock info
            for (size_t i = 0; i < gameData.moves.size(); ++i) {
                std::string clockStr;
                
                // Clocks array typically contains the initial state at [0], so the clock after move i is at i + 1
                size_t clockIdx = i + 1;
                const auto* clk_ptr = (clockIdx < gameData.clocks.size()) ? &gameData.clocks[clockIdx] : 
                                      (i < gameData.clocks.size()) ? &gameData.clocks[i] : nullptr;

                if (clk_ptr) {
                    // Even 'i' means White's move, odd 'i' means Black's move
                    clockStr = (i % 2 == 0) ? clk_ptr->white_time : clk_ptr->black_time;
                } else {
                    clockStr = "0:00:00"; // Fallback if no clock data exists for this ply
                }
                std::string move_with_annotation = gameData.moves[i] + move_annotations[i];
                pgn.add_ply(move_with_annotation, format_clock_string(clockStr));

                // Check for and add variations that branch from this move
                auto it = gameData.variations.find(i);
                if (it != gameData.variations.end()) {
                    const auto& vars_at_ply = it->second;
                    for (const auto& var_data : vars_at_ply) {
                        pgn.push_variation();
                        for (size_t j = 0; j < var_data.moves.size(); ++j) {
                            std::string var_clock_str = "0:00:00";
                            if (j < var_data.clocks.size()) {
                                const auto& clk = var_data.clocks[j];
                                // The j-th move in the variation has ply index (i + j)
                                var_clock_str = ((i + j) % 2 == 0) ? clk.white_time : clk.black_time;
                            }
                            
                            std::string eval_str = "";
                            if (settings.enableStockfish && j + 1 < var_data.fens.size()) {
                                auto cache_it = fenAnalysisCache.find(var_data.fens[j + 1]);
                                if (cache_it != fenAnalysisCache.end()) {
                                    eval_str = get_eval_str(cache_it->second);
                                }
                            }
                            pgn.add_ply(var_data.moves[j], format_clock_string(var_clock_str), eval_str);
                        }
                        pgn.pop_variation();
                    }
                }
            }

            // Inject Stockfish analysis if requested for PGN
            if (settings.enableStockfish && !mainLineStockfishResults.empty()) {
                pgn.add_stockfish_analysis(mainLineStockfishResults, settings.stockfishAnalysisDepth);
            }

            std::string pgnContent = pgn.build();

            // Write to file
            QFileInfo outInfo(settings.outputPath);
            QString pgnPath = outInfo.absoluteDir().filePath(outInfo.completeBaseName() + ".pgn");
            QDir().mkpath(outInfo.absolutePath()); // Ensure output directory exists
            std::ofstream pgnFile(pgnPath.toStdString());
            if (pgnFile.is_open()) {
                pgnFile << pgnContent;
                pgnFile.close();
                emit logMessage("PGN file written to: " + pgnPath);
            } else {
                emit logMessage("Warning: Could not write PGN file.");
            }
        }

        // Step 4: Optional Analysis Video Generation
        if (settings.generateAnalysisVideo) {
            emit logMessage("Starting Analysis Video generation...");
            
            QSettings qs;
            QString ext = qs.value("videoExtension", ".mp4").toString();
            QString vCodec = qs.value("videoCodec", "libx264 (H.264)").toString();
            QString aCodec = qs.value("audioCodec", "aac").toString();
            QString res = qs.value("videoResolution", "Source Resolution").toString();
            QString crf = qs.value("videoQuality", 23).toString();
            QString arrows = qs.value("videoAnalysisArrows", "Debug Board").toString();
            emit logMessage(QString("Using video encoding settings: Codec=%1, Audio=%2, Container=%3, Resolution=%4, Quality (CRF)=%5, Arrows=%6").arg(vCodec).arg(aCodec).arg(ext).arg(res).arg(crf).arg(arrows));

            try {
                AnalysisVideoGenerator generator(settings.assetsPath.toStdString());
                
                QFileInfo outInfo(settings.outputPath);
                QString analysisVideoPath = outInfo.absoluteDir().filePath(outInfo.completeBaseName() + "_analysis" + ext);

                auto generator_progress_callback = [this](int percent, const std::string& msg) {
                    // This callback is called from within the generator's loop.
                    // The loop itself will check the cancel flag, so we don't
                    // need to check it here, but we could if we wanted to
                    // perform a specific action on cancel during progress update.
                    // For now, just log messages. A more complex progress system could scale this.
                    if (percent >= 0) {
                        emit progressUpdated(percent);
                    }
                    if (!msg.empty()) {
                        emit logMessage(QString::fromStdString(msg));
                    }
                };

                const BoardGeometry* geo = extractor.get_board_geometry();
                if (!geo) {
                    emit logMessage("Error: Board geometry not available for Analysis Video generation.");
                } else {
                    // Pack the UI codec settings into the path string to seamlessly pass them 
                    // without altering the underlying C++ interface signature.
                    QString magicVideoPath = analysisVideoPath + "|" + vCodec + "|" + aCodec + "|" + res + "|" + crf + "|||" + arrows;

                    bool success = generator.generate_analysis_video(
                        settings.videoPath.toStdString(),
                        magicVideoPath.toStdString(),
                        *geo,
                        gameData.video_fens,
                        gameData.video_timestamps,
                        videoStockfishResults,
                        15, // Hardcoded engine arrow thickness percentage
                        settings.overlayConfig,
                        cancelFlag,
                        generator_progress_callback
                    );

                    if (success) {
                        emit logMessage("Successfully generated Analysis Video: " + analysisVideoPath);
                    } else {
                        emit logMessage("Error: Failed to generate Analysis Video.");
                    }
                }

            } catch (const std::exception& e) {
                emit logMessage("Error during Analysis Video generation: " + QString::fromStdString(e.what()));
            }
        }

        if (cancelFlag && *cancelFlag) {
            emit finished();
            return;
        }

        emit finished();

    } catch (const std::exception& e) {
        QString errorMsg = QString::fromStdString(e.what());
        emit logMessage("Error: " + errorMsg);
        emit error(errorMsg);
    } catch (...) {
        QString errorMsg = "An unknown error occurred during video processing.";
        emit logMessage(errorMsg);
        emit error(errorMsg);
    }
}

} // namespace aa