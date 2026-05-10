// Extracted from cpp directory
#include "VideoProcessorWorker.h"
#include "ChessVideoExtractor.h"
#include "AnalysisVideoGenerator.h"
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"
#include "BoardLocalizer.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include "OpeningFetcher.h"

#include <QSettings>
#include <QCoreApplication>
#include <exception>
#include <fstream>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <map>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cta {

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

    std::string format_srt_timestamp(double seconds) {
        if (seconds < 0.0) {
            seconds = 0.0;
        }

        const long long total_ms = static_cast<long long>(std::llround(seconds * 1000.0));
        const long long ms = total_ms % 1000;
        const long long total_seconds = total_ms / 1000;
        const long long s = total_seconds % 60;
        const long long total_minutes = total_seconds / 60;
        const long long m = total_minutes % 60;
        const long long h = total_minutes / 60;

        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setfill('0')
            << std::setw(2) << h << ":"
            << std::setw(2) << m << ":"
            << std::setw(2) << s << ","
            << std::setw(3) << ms;
        return out.str();
    }

    std::string move_to_subtitle_text(size_t ply_index, const std::string& san_move) {
        std::ostringstream out;
        const size_t move_number = (ply_index / 2) + 1;
        if (ply_index % 2 == 0) {
            out << move_number << ". " << san_move;
        } else {
            out << move_number << "... " << san_move;
        }
        return out.str();
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
        std::string arrows_target = settings.overlayConfig.arrowsTarget;
        if (arrows_target.empty()) arrows_target = "Analysis Board";
        if (arrows_target == "Debug Board") arrows_target = "Analysis Board";

        const bool analysisVideoNeedsEngine =
            settings.generateAnalysisVideo &&
            (settings.overlayConfig.evalBar.enabled ||
             settings.overlayConfig.pvText.enabled ||
             ((settings.overlayConfig.board.enabled || arrows_target == "Main Board" || arrows_target == "Both") &&
              (arrows_target == "Analysis Board" || arrows_target == "Main Board" || arrows_target == "Both")));
        const bool runStockfishAnalysis = settings.enableMoveAnnotations || analysisVideoNeedsEngine;

        emit logMessage(
            QString("Overlay config: board(enabled=%1, x=%2, y=%3, scale=%4), "
                    "eval(enabled=%5, x=%6, y=%7, scale=%8), "
                    "pv(enabled=%9, x=%10, y=%11, scale=%12), arrows=%13")
                .arg(settings.overlayConfig.board.enabled ? "true" : "false")
                .arg(settings.overlayConfig.board.x_percent, 0, 'f', 3)
                .arg(settings.overlayConfig.board.y_percent, 0, 'f', 3)
                .arg(settings.overlayConfig.board.scale, 0, 'f', 3)
                .arg(settings.overlayConfig.evalBar.enabled ? "true" : "false")
                .arg(settings.overlayConfig.evalBar.x_percent, 0, 'f', 3)
                .arg(settings.overlayConfig.evalBar.y_percent, 0, 'f', 3)
                .arg(settings.overlayConfig.evalBar.scale, 0, 'f', 5)
                .arg(settings.overlayConfig.pvText.enabled ? "true" : "false")
                .arg(settings.overlayConfig.pvText.x_percent, 0, 'f', 3)
                .arg(settings.overlayConfig.pvText.y_percent, 0, 'f', 3)
                .arg(settings.overlayConfig.pvText.scale, 0, 'f', 3)
                .arg(QString::fromStdString(settings.overlayConfig.arrowsTarget))
        );

        if (analysisVideoNeedsEngine && !settings.enableMoveAnnotations) {
            emit logMessage("Analysis Video overlays require engine data; running Stockfish analysis for video overlays.");
        }

        if (settings.generateAnalysisVideo) {
            emit logMessage("Verifying FFmpeg installation...");
            if (!is_ffmpeg_available()) {
                throw std::runtime_error("FFmpeg is required for Analysis Video generation but was not found in the system PATH. Please install FFmpeg and restart the application.");
            }
        }

        if (runStockfishAnalysis) {
            emit logMessage("Verifying Stockfish installation...");
            QString sfPath = settings.stockfishPath;
            bool sfFound = false;

            if (!sfPath.isEmpty()) {
                if (QFileInfo(sfPath).isExecutable()) {
                    sfFound = true;
                } else if (!QStandardPaths::findExecutable(sfPath).isEmpty()) {
                    sfFound = true;
                }
            } else {
                QString execName = "stockfish";
#ifdef _WIN32
                execName = "stockfish.exe";
#endif
                QStringList pathsToCheck = {
                    "stockfish/" + execName,
                    "../stockfish/" + execName,
                    "../../stockfish/" + execName
                };
                for (const QString& p : pathsToCheck) {
                    if (QFileInfo(p).isExecutable()) {
                        sfFound = true;
                        break;
                    }
                }

                if (!sfFound && !QStandardPaths::findExecutable(execName).isEmpty()) {
                    sfFound = true;
                }
            }

            if (!sfFound) {
                throw std::runtime_error("Stockfish executable is required but was not found. Please specify its location in Settings -> Stockfish -> Stockfish location.");
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

        OpeningFetcher opening_fetcher;
        extractor.set_fen_detected_callback([&opening_fetcher](const std::string& fen) {
            opening_fetcher.enqueue_fen(fen);
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

        if (runStockfishAnalysis && !gameData.fens.empty()) {
            // 1. Gather all unique FENs from the video timeline
            std::vector<std::string> unique_fens;
            for (const auto& fen : gameData.video_fens) {
                if (std::find(unique_fens.begin(), unique_fens.end(), fen) == unique_fens.end()) {
                    unique_fens.push_back(fen);
                }
            }
            
            QString limitsStr = QString("Depth=%1").arg(settings.stockfishDepth);
            if (settings.stockfishTime > 0) limitsStr += QString(", Time=%1s").arg(settings.stockfishTime);
            if (settings.stockfishNodes > 0) limitsStr += QString(", Nodes=%1").arg(settings.stockfishNodes);
            
            emit logMessage("Starting Stockfish analysis (MultiPV=" + QString::number(settings.multiPv) + ", " + limitsStr + ", " + QString::number(unique_fens.size()) + " unique positions)...");

            StockfishAnalyzer analyzer(settings.multiPv, settings.stockfishPath.toStdString());
            analyzer.set_progress_callback([this, cancelFlag](int current, int total) {
                QString msg = QString("Analyzing position %1 of %2...").arg(current).arg(total);
                emit logMessage(msg);
                if (total > 0) {
                    emit progressUpdated((current * 100) / total);
                }
            });
            std::vector<StockfishResult> unique_results = analyzer.analyze_positions(unique_fens, settings.stockfishDepth, settings.stockfishTime * 1000, settings.stockfishNodes, cancelFlag);
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
            mainLineStockfishResults.reserve(gameData.fens.size());
            for (const auto& fen : gameData.fens) {
                mainLineStockfishResults.push_back(fenAnalysisCache[fen]);
            }
            videoStockfishResults.reserve(gameData.video_fens.size());
            for (const auto& fen : gameData.video_fens) {
                videoStockfishResults.push_back(fenAnalysisCache[fen]);
            }
            
            emit logMessage("Stockfish analysis complete.");
        }

        // Step 2a: Optional move quality annotation based on Stockfish analysis
        std::vector<std::string> move_annotations(gameData.moves.size(), "");
        int white_estimated_elo = 0;
        int black_estimated_elo = 0;
        double white_acpl = -1.0;
        double black_acpl = -1.0;

        if (settings.enableMoveAnnotations && runStockfishAnalysis) {
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

                    cta::StockfishLine dummy;
                    dummy.move_uci = "ANNOTATION";
                    dummy.pv_line = gameData.video_moves[i] + video_ann;
                    videoStockfishResults[i + 1].lines.push_back(dummy);
                }
            }

            // Annotate Main Line Moves (for PGN) only when engine PGN output was requested.
            if (settings.generatePgn && settings.enableMoveAnnotations && !mainLineStockfishResults.empty() && mainLineStockfishResults.size() > gameData.moves.size()) {
                double total_loss_white = 0.0;
                double total_loss_black = 0.0;
                int moves_white = 0;
                int moves_black = 0;

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

                    // Cap centipawn loss at 1000 (10 pawns) so a single missed mate doesn't completely destroy the average
                    int capped_loss = std::min(cp_loss, 1000);
                    if (is_white_to_move) {
                        total_loss_white += capped_loss;
                        moves_white++;
                    } else {
                        total_loss_black += capped_loss;
                        moves_black++;
                    }

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

                if (moves_white > 0 || moves_black > 0) {
                    int depth = settings.stockfishDepth;
                    // Penalize base Elo and increase CP loss impact for lower engine depths
                    double depth_multiplier = 10.0 + std::max(0, 20 - depth) * 0.5;
                    int base_elo = 3000 - std::max(0, 20 - depth) * 50;
                    
                    if (moves_white > 0) {
                        white_acpl = total_loss_white / moves_white;
                        white_estimated_elo = std::clamp(static_cast<int>(base_elo - white_acpl * depth_multiplier), 100, 3000);
                    }
                    if (moves_black > 0) {
                        black_acpl = total_loss_black / moves_black;
                        black_estimated_elo = std::clamp(static_cast<int>(base_elo - black_acpl * depth_multiplier), 100, 3000);
                    }
                    
                    double w_acc = white_acpl >= 0 ? std::clamp(100.0 * std::exp(-0.007 * white_acpl), 0.0, 100.0) : 0.0;
                    double b_acc = black_acpl >= 0 ? std::clamp(100.0 * std::exp(-0.007 * black_acpl), 0.0, 100.0) : 0.0;

                    emit logMessage(QString("Estimated Performance:") + 
                        QString("\n  White: ~%1 Elo | %2% Accuracy | %3 ACPL").arg(white_estimated_elo).arg(w_acc, 0, 'f', 1).arg(white_acpl >= 0 ? white_acpl : 0.0, 0, 'f', 1) +
                        QString("\n  Black: ~%1 Elo | %2% Accuracy | %3 ACPL").arg(black_estimated_elo).arg(b_acc, 0, 'f', 1).arg(black_acpl >= 0 ? black_acpl : 0.0, 0, 'f', 1));
                }
            }
        }

        emit logMessage("Waiting for background Lichess opening data to finish...");

        std::atomic<bool> fetcher_done{false};
        std::thread monitor_thread([this, &fetcher_done, cancelFlag, &opening_fetcher, &gameData]() {
            int elapsed_ms = 0;
            size_t fen_idx = 0;
            int last_reported_s = 0;

            while (!fetcher_done) {
                // Sleep in small increments to respond quickly to fetcher_done or cancellation
                for (int i = 0; i < 5 && !fetcher_done; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    elapsed_ms += 100;
                    if (cancelFlag && *cancelFlag) break;
                }
                if (fetcher_done || (cancelFlag && *cancelFlag)) break;

                // Look for newly completed queries in the OpeningFetcher cache
                while (fen_idx < gameData.video_fens.size()) {
                    LichessOpening info = opening_fetcher.get_opening(gameData.video_fens[fen_idx]);
                    if (info.total_games == -1) break; // Not fetched yet

                    QString move_str = (fen_idx < gameData.video_moves.size()) ? QString::fromStdString(gameData.video_moves[fen_idx]) : QString("FEN %1").arg(fen_idx);

                    if (info.api_success || info.total_games > 0) {
                        emit logMessage(QString("Lichess checked %1 -> %2 hits").arg(move_str).arg(info.total_games));
                    } else {
                        QString detail = info.error.empty() ? "API request failed" : QString::fromStdString(info.error);
                        emit logMessage(QString("Lichess checked %1 -> %2").arg(move_str, detail));
                    }

                    fen_idx++;
                    if (info.total_games <= 1 && info.api_success) break; // Unique game reached, fetcher stopped
                }

                int elapsed_s = elapsed_ms / 1000;
                if (elapsed_s >= last_reported_s + 5) {
                    QString waiting_on = (fen_idx < gameData.video_moves.size()) ? QString::fromStdString(gameData.video_moves[fen_idx]) : "completion";
                    emit logMessage(QString("Still fetching Lichess opening data... (%1s elapsed, waiting on %2)").arg(elapsed_s).arg(waiting_on));
                    last_reported_s = elapsed_s;
                }
            }
        });

        opening_fetcher.wait_until_done();
        fetcher_done = true;
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        
        std::string final_eco;
        std::string final_opening_name;
        std::vector<std::string> video_opening_names(gameData.video_fens.size(), "");
        for (size_t i = 0; i < gameData.video_fens.size(); ++i) {
            LichessOpening info = opening_fetcher.get_opening(gameData.video_fens[i]);
            if (info.found) {
                final_eco = info.eco;
                final_opening_name = info.name;
            }
            video_opening_names[i] = final_eco.empty() ? "" : (final_eco + " " + final_opening_name);
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
            pgn.add_header("White", white_estimated_elo > 0 ? "Unknown (~" + std::to_string(white_estimated_elo) + ")" : "Unknown");
            pgn.add_header("Black", black_estimated_elo > 0 ? "Unknown (~" + std::to_string(black_estimated_elo) + ")" : "Unknown");
            
            if (white_estimated_elo > 0) pgn.add_header("WhiteElo", std::to_string(white_estimated_elo));
            if (black_estimated_elo > 0) pgn.add_header("BlackElo", std::to_string(black_estimated_elo));
            if (!final_eco.empty()) {
                pgn.add_header("ECO", final_eco);
                pgn.add_header("Opening", final_opening_name);
            }

            // Add exact ACPL and an approximated Accuracy % to custom PGN headers
            if (white_acpl >= 0) {
                pgn.add_header("WhiteACPL", std::to_string(static_cast<int>(std::round(white_acpl))));
                double w_acc = std::clamp(100.0 * std::exp(-0.007 * white_acpl), 0.0, 100.0);
                std::ostringstream ss;
                ss.imbue(std::locale::classic());
                ss << std::fixed << std::setprecision(1) << w_acc;
                pgn.add_header("WhiteAccuracy", ss.str());
            }
            if (black_acpl >= 0) {
                pgn.add_header("BlackACPL", std::to_string(static_cast<int>(std::round(black_acpl))));
                double b_acc = std::clamp(100.0 * std::exp(-0.007 * black_acpl), 0.0, 100.0);
                std::ostringstream ss;
                ss.imbue(std::locale::classic());
                ss << std::fixed << std::setprecision(1) << b_acc;
                pgn.add_header("BlackAccuracy", ss.str());
            }

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
                std::ostringstream ss;
                ss.imbue(std::locale::classic());
                ss << std::showpos << std::fixed << std::setprecision(2) << eval_cp;
                return ss.str();
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
                
                std::string main_eval_str = "";
                if (settings.generatePgn && settings.enableMoveAnnotations && !mainLineStockfishResults.empty() && i + 1 < mainLineStockfishResults.size()) {
                    main_eval_str = get_eval_str(mainLineStockfishResults[i + 1]);
                }

                std::string move_with_annotation = gameData.moves[i] + move_annotations[i];
                pgn.add_ply(move_with_annotation, format_clock_string(clockStr), main_eval_str);

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
                            if (settings.generatePgn && settings.enableMoveAnnotations && j + 1 < var_data.fens.size()) {
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
            
            QString outPath = settings.outputPath;
            std::string pgnContent = pgn.build();
            std::ofstream pgnFile(outPath.toStdString());
            if (pgnFile.is_open()) {
                pgnFile << pgnContent;
                pgnFile.close();
                emit logMessage("Saved PGN to: " + outPath);
            } else {
                emit error(QString("Failed to save PGN to: %1").arg(outPath));
                return;
            }
        }

        QFileInfo pgnInfo(settings.outputPath);
        
        QSettings q_settings_vid;
        QString vCodec = q_settings_vid.value("videoCodec", "libx264").toString();
        QString aCodec = q_settings_vid.value("audioCodec", "copy").toString();
        QString resolution = q_settings_vid.value("videoResolution", "Source Resolution").toString();
        QString crf = q_settings_vid.value("videoQuality", "23").toString();
        QString extension = q_settings_vid.value("videoExtension", ".mp4").toString();

        QString outPath = QDir(pgnInfo.absolutePath()).filePath(QFileInfo(settings.videoPath).completeBaseName() + "_analysis" + extension);
        QString subtitlePath;

        if (settings.generateSubtitles) {
            emit logMessage("Generating synced move subtitles...");

            QDir().mkpath(pgnInfo.absolutePath());
            subtitlePath = outPath + ".srt";

            std::ofstream srtFile(subtitlePath.toStdString());
            if (!srtFile.is_open()) {
                emit error(QString("Failed to save subtitles to: %1").arg(subtitlePath));
                return;
            }

            libchess::Position subtitlePos(gameData.fens.empty() ? "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" : gameData.fens.front());
            const size_t cueCount = std::min(gameData.moves.size(), gameData.timestamps.size());
            for (size_t i = 0; i < cueCount; ++i) {
                const std::string& uci = gameData.moves[i];
                std::string san = uci;

                try {
                    libchess::Move move = subtitlePos.parse_move(uci);
                    san = build_san(subtitlePos, move, uci);
                    subtitlePos.makemove(move);
                } catch (const std::exception& e) {
                    emit logMessage(QString("Warning: subtitle notation fallback for %1: %2")
                                        .arg(QString::fromStdString(uci))
                                        .arg(e.what()));
                }

                const double start = gameData.timestamps[i];
                double end = start + 2.5;
                if (i + 1 < gameData.timestamps.size()) {
                    end = std::max(start + 0.75, gameData.timestamps[i + 1]);
                }

                srtFile << (i + 1) << "\n"
                        << format_srt_timestamp(start) << " --> " << format_srt_timestamp(end) << "\n"
                        << move_to_subtitle_text(i, san) << "\n\n";
            }

            srtFile.close();
        }

        // Step 4: Optional Analysis Video Generation
        if (settings.generateAnalysisVideo) {
            emit logMessage("Generating Analysis Video...");
            AnalysisVideoGenerator video_gen(settings.assetsPath.toStdString());
            
            auto progress_cb = [this](int percent, const std::string& message) {
                if (percent >= 0) {
                    emit progressUpdated(percent);
                }
                if (!message.empty()) {
                    emit logMessage(QString::fromStdString(message));
                }
            };

            QString outPathWithArgs = outPath + "|" + vCodec + "|" + aCodec + "|" + resolution + "|" + crf;
            bool videoGenerated = video_gen.generate_analysis_video(
                settings.videoPath.toStdString(),
                outPathWithArgs.toStdString(),
                *extractor.get_board_geometry(),
                gameData.video_fens,
                gameData.video_timestamps,
                videoStockfishResults,
                video_opening_names,
                15, // arrow_thickness_pct
                settings.overlayConfig,
                cancelFlag,
                progress_cb
            );

            // Automatic fallback to CPU encoder if GPU hardware acceleration fails
            if (!videoGenerated && !(cancelFlag && *cancelFlag) && vCodec != "libx264" && vCodec != "libx265" && vCodec != "libvpx-vp9") {
                emit logMessage("Hardware accelerated composition failed. Retrying with H.264 CPU encoder (libx264)...");
                outPathWithArgs = outPath + "|libx264|" + aCodec + "|" + resolution + "|" + crf;
                videoGenerated = video_gen.generate_analysis_video(
                    settings.videoPath.toStdString(),
                    outPathWithArgs.toStdString(),
                    *extractor.get_board_geometry(),
                    gameData.video_fens,
                    gameData.video_timestamps,
                    videoStockfishResults,
                    video_opening_names,
                    15,
                    settings.overlayConfig,
                    cancelFlag,
                    progress_cb
                );
                
                if (!videoGenerated && !(cancelFlag && *cancelFlag)) {
                    emit logMessage("Error: Software CPU encoder fallback also failed. Please check the logs above.");
                }
            }

            if (settings.generateSubtitles) {
                QFile::remove(subtitlePath);
            }

            if (!videoGenerated) {
                if (cancelFlag && *cancelFlag) {
                    emit finished();
                } else {
                    emit error("Analysis Video generation failed. See the log above for details.");
                }
                return;
            }

            emit logMessage("Analysis Video generation complete: " + outPath);
        } else if (settings.generateSubtitles) {
            // Cleanup stray temp subtitle files if video generation was bypassed programmatically
            QFile::remove(subtitlePath);
        }

        emit progressUpdated(100);
        emit logMessage("Processing complete.");
        emit finished();

    } catch (const std::exception& e) {
        emit error(QString("Error during processing: %1").arg(e.what()));
    } catch (...) {
        emit error("Unknown error during processing.");
    }
}

} // namespace cta
