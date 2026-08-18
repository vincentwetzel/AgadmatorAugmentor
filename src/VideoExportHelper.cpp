#include "VideoExportHelper.h"
#include "VideoProcessorWorker.h" // For GameData
#include "ChessVideoExtractor.h"
#include "VideoProcessorWorker_Utils.h"
#include "PgnWriter.h"
#include "AnalysisVideoGenerator.h"
#include "ChessFenUtils.h"
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace cta {

VideoExportHelper::VideoExportHelper(QObject* parent) : QObject(parent) {}

void VideoExportHelper::verifyEnvironment(const ProcessingSettings& settings, bool runStockfishAnalysis) {
    if (settings.generateAnalysisVideo) {
        emit logMessage("Verifying FFmpeg installation...");
        if (!Utils::is_ffmpeg_available()) {
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
}

bool VideoExportHelper::exportAll(const ProcessingSettings& settings,
                                  const GameData& gameData,
                                  const StockfishAnalysisResults& sfResults,
                                  const LichessSyncResults& lichessResults,
                                  const BoardGeometry& geo,
                                  std::atomic<bool>* cancelFlag) {
    if (settings.generatePgn) {
        emit logMessage("Generating PGN file...");
        if (!generatePgn(settings, gameData, sfResults, lichessResults)) return false;
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
        if (!generateSubtitles(settings, gameData, subtitlePath)) return false;
    }

    bool success = true;
    if (settings.generateAnalysisVideo) {
        emit logMessage("Generating Analysis Video...");
        success = generateVideo(settings, gameData, sfResults, lichessResults, geo, outPath, subtitlePath, cancelFlag);
    }

    if (settings.generateSubtitles && !subtitlePath.isEmpty()) {
        QFile::remove(subtitlePath); // Cleanup the temporary subtitle file once embedded
    }

    return success;
}

bool VideoExportHelper::generatePgn(const ProcessingSettings& settings, const GameData& gameData, const StockfishAnalysisResults& sfResults, const LichessSyncResults& lichessResults) {
    PgnWriter pgn;
    pgn.add_header("Event", "Unknown");
    pgn.add_header("Site", "Unknown");
    pgn.add_header("Date", "Unknown");
    pgn.add_header("Round", "Unknown");
    pgn.add_header("White", sfResults.whiteEstimatedElo > 0 ? "Unknown (~" + std::to_string(sfResults.whiteEstimatedElo) + ")" : "Unknown");
    pgn.add_header("Black", sfResults.blackEstimatedElo > 0 ? "Unknown (~" + std::to_string(sfResults.blackEstimatedElo) + ")" : "Unknown");
    
    if (sfResults.whiteEstimatedElo > 0) pgn.add_header("WhiteElo", std::to_string(sfResults.whiteEstimatedElo));
    if (sfResults.blackEstimatedElo > 0) pgn.add_header("BlackElo", std::to_string(sfResults.blackEstimatedElo));
    if (!lichessResults.finalEco.empty()) {
        pgn.add_header("ECO", lichessResults.finalEco);
        pgn.add_header("Opening", lichessResults.finalOpeningName);
    }

    if (sfResults.whiteAcpl >= 0) {
        pgn.add_header("WhiteACPL", std::to_string(static_cast<int>(std::round(sfResults.whiteAcpl))));
        double w_acc = std::clamp<double>(100.0 * std::exp(-0.007 * sfResults.whiteAcpl), 0.0, 100.0);
        std::ostringstream ss; ss.imbue(std::locale::classic()); ss << std::fixed << std::setprecision(1) << w_acc;
        pgn.add_header("WhiteAccuracy", ss.str());
    }
    if (sfResults.blackAcpl >= 0) {
        pgn.add_header("BlackACPL", std::to_string(static_cast<int>(std::round(sfResults.blackAcpl))));
        double b_acc = std::clamp<double>(100.0 * std::exp(-0.007 * sfResults.blackAcpl), 0.0, 100.0);
        std::ostringstream ss; ss.imbue(std::locale::classic()); ss << std::fixed << std::setprecision(1) << b_acc;
        pgn.add_header("BlackAccuracy", ss.str());
    }

    for (size_t i = 0; i < gameData.moves.size(); ++i) {
        std::string clockStr;
        size_t clockIdx = i + 1;
        const auto* clk_ptr = (clockIdx < gameData.clocks.size()) ? &gameData.clocks[clockIdx] : (i < gameData.clocks.size()) ? &gameData.clocks[i] : nullptr;
        clockStr = clk_ptr ? ((i % 2 == 0) ? clk_ptr->white_time : clk_ptr->black_time) : "0:00:00";
        
        std::string main_eval_str = (settings.includePgnAnalysis && !sfResults.mainLineResults.empty() && i + 1 < sfResults.mainLineResults.size() && !sfResults.mainLineResults[i + 1].lines.empty())
            ? ChessFenUtils::format_eval_string(sfResults.mainLineResults[i + 1].lines[0], sfResults.mainLineResults[i + 1].fen) : "";
        
        std::string move_with_annotation = gameData.moves[i];
        if (settings.includePgnMoveAnnotations && i < sfResults.moveAnnotations.size()) {
            move_with_annotation += sfResults.moveAnnotations[i];
        }
        pgn.add_ply(move_with_annotation, Utils::format_clock_string(clockStr), main_eval_str);

        auto it = gameData.variations.find(i);
        if (it != gameData.variations.end()) {
            for (const auto& var_data : it->second) {
                pgn.push_variation();
                for (size_t j = 0; j < var_data.moves.size(); ++j) {
                    std::string var_clock_str = "0:00:00";
                    if (j < var_data.clocks.size()) var_clock_str = ((i + j) % 2 == 0) ? var_data.clocks[j].white_time : var_data.clocks[j].black_time;
                    std::string eval_str = "";
                    if (settings.includePgnAnalysis && j + 1 < var_data.fens.size()) {
                        auto cache_it = sfResults.fenCache.find(var_data.fens[j + 1]);
                        if (cache_it != sfResults.fenCache.end() && !cache_it->second.lines.empty()) eval_str = ChessFenUtils::format_eval_string(cache_it->second.lines[0], cache_it->second.fen);
                    }
                    pgn.add_ply(var_data.moves[j], Utils::format_clock_string(var_clock_str), eval_str);
                }
                pgn.pop_variation();
            }
        }
    }
    
    std::ofstream pgnFile(settings.outputPath.toStdString());
    if (pgnFile.is_open()) {
        pgnFile << pgn.build();
        emit logMessage("Saved PGN to: " + settings.outputPath);
        return true;
    }
    emit error(QString("Failed to save PGN to: %1").arg(settings.outputPath));
    return false;
}

bool VideoExportHelper::generateSubtitles(const ProcessingSettings& settings, const GameData& gameData, const QString& subtitlePath) {
    std::ofstream srtFile(subtitlePath.toStdString());
    if (!srtFile.is_open()) {
        emit error(QString("Failed to save subtitles to: %1").arg(subtitlePath));
        return false;
    }

    int sub_index = 1;
    for (size_t i = 0; i < gameData.video_timestamps.size(); ++i) {
        if (gameData.video_moves[i] == "REVERT") continue;

        double start_t = gameData.video_timestamps[i];
        // Revert/replay reduction can leave a stale timestamp adjacent to a
        // restored move.  Never emit a non-finite timestamp: FFmpeg's SRT
        // demuxer cannot represent it and may pass an invalid packet duration
        // to the MP4 muxer.
        if (!std::isfinite(start_t)) continue;
        start_t = std::max(0.0, start_t);

        double end_t = start_t + 5.0; // Default 5 seconds duration

        // Cap the subtitle duration when the next move happens
        for (size_t j = i + 1; j < gameData.video_timestamps.size(); ++j) {
            if (gameData.video_moves[j] != "REVERT") {
                const double next_t = gameData.video_timestamps[j];
                // The next logical move is not necessarily later after an
                // analysis branch is restored.  Using it unconditionally can
                // create an SRT cue with a negative duration, which FFmpeg
                // later reports as a huge wrapped duration (for example
                // 4294682796000) while muxing MP4 subtitles.
                if (std::isfinite(next_t) && next_t > start_t) {
                    end_t = std::min(end_t, next_t);
                    break;
                }
            }
        }

        if (!(end_t > start_t)) continue;

        std::string fen = gameData.video_fens[i];
        
        int full_move = 1;
        bool is_white = true;
        std::istringstream fen_ss(fen);
        std::vector<std::string> fen_parts;
        std::string part;
        while (fen_ss >> part) {
            fen_parts.push_back(part);
        }
        if (fen_parts.size() >= 2) {
            is_white = (fen_parts[1] == "w");
        } else {
            is_white = (fen.find(" w ") != std::string::npos);
        }
        if (fen_parts.size() >= 6) {
            try { full_move = std::stoi(fen_parts[5]); } catch (...) {}
        }

        // Use 1-based ply index to map correctly to PgnWriter-style move numbers
        int ply_index = (full_move - 1) * 2 + (is_white ? 1 : 2);
        std::string san_move = ChessFenUtils::uci_to_san_line(gameData.video_moves[i], fen);

        srtFile << sub_index++ << "\n";
        srtFile << Utils::format_srt_timestamp(start_t) << " --> " << Utils::format_srt_timestamp(end_t) << "\n";
        srtFile << Utils::move_to_subtitle_text(ply_index, san_move) << "\n\n";
    }

    // FFmpeg fails if the SRT file is completely empty
    if (sub_index == 1) {
        srtFile << "1\n";
        srtFile << "00:00:00,000 --> 00:00:01,000\n";
        srtFile << "Analysis Started\n\n";
    }

    return true; 
}

bool VideoExportHelper::generateVideo(const ProcessingSettings& settings, const GameData& gameData, const StockfishAnalysisResults& sfResults, const LichessSyncResults& lichessResults, const BoardGeometry& geo, const QString& outPath, const QString& subtitlePath, std::atomic<bool>* cancelFlag) {
    AnalysisVideoGenerator generator(settings.assetsPath.toStdString());
    
    bool has_error = false;
    auto progress_cb = [this, &has_error](int percent, const std::string& msg) {
        if (percent >= 0) {
            emit progressUpdated(percent);
            if (!msg.empty()) {
                emit logMessage(QString::fromStdString(msg));
            }
        } else {
            has_error = true;
            emit error(QString::fromStdString(msg));
        }
    };

    QSettings q_settings_vid;
    QString vCodec = q_settings_vid.value("videoCodec", "libx264").toString();
    QString aCodec = q_settings_vid.value("audioCodec", "copy").toString();
    QString resolution = q_settings_vid.value("videoResolution", "Source Resolution").toString();
    QString crf = q_settings_vid.value("videoQuality", "23").toString();
    
    QString fullOutPath = outPath + "|" + vCodec + "|" + aCodec + "|" + resolution + "|" + crf;

    bool success = generator.generate_analysis_video(
        settings.videoPath.toStdString(),
        fullOutPath.toStdString(),
        geo,
        gameData.video_fens,
        gameData.video_timestamps,
        sfResults.videoResults,
        lichessResults.videoOpeningNames,
        settings.overlayConfig.arrowThicknessPct, // arrow thickness pct
        settings.overlayConfig,
        cancelFlag,
        progress_cb
    );

    return success && !has_error;
}

} // namespace cta
