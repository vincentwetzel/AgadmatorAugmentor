// Extracted from cpp directory
#include "VideoProcessorWorker.h"
#include "ChessVideoExtractor.h"
#include "StockfishAnalyzer.h"
#include "VideoExportHelper.h"
#include "VideoProcessorWorker_Utils.h" // Include the new utility header
#include "ChessFenUtils.h"
#include "BoardLocalizer.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"
#include "LichessSyncHelper.h"

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
        const bool runStockfishAnalysis = settings.includePgnAnalysis ||
            settings.includePgnMoveAnnotations || settings.enableMoveAnnotations || analysisVideoNeedsEngine;

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

        VideoExportHelper exportHelper;
        connect(&exportHelper, &VideoExportHelper::logMessage, this, &VideoProcessorWorker::logMessage, Qt::DirectConnection);
        connect(&exportHelper, &VideoExportHelper::progressUpdated, this, &VideoProcessorWorker::progressUpdated, Qt::DirectConnection);
        connect(&exportHelper, &VideoExportHelper::error, this, &VideoProcessorWorker::error, Qt::DirectConnection);

        exportHelper.verifyEnvironment(settings, runStockfishAnalysis);

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

        emit logMessage("Verifying Lichess API connection...");
        LichessSyncHelper lichessHelper;
        connect(&lichessHelper, &LichessSyncHelper::logMessage, this, &VideoProcessorWorker::logMessage, Qt::DirectConnection);
        emit logMessage("Lichess API connection verified successfully.");
        
        extractor.set_fen_detected_callback(lichessHelper.getFenCallback());

        // Step 1: Extract moves from video. Keep the stage boundaries explicit
        // because the per-video job log is the first place users look after a
        // successful run as well as after a failure.
        emit logMessage("Stage 1/4: extracting and verifying chess moves.");
        GameData gameData = extractor.extract_moves_from_video(
            settings.videoPath.toStdString(),
            "", // debug_label
            cancelFlag
        );

        size_t variationCount = 0;
        for (const auto& entry : gameData.variations) {
            variationCount += entry.second.size();
        }
        emit logMessage(QString("Move extraction complete: %1 main-line plies, %2 variation(s), %3 clock record(s).")
            .arg(gameData.moves.size())
            .arg(variationCount)
            .arg(gameData.clocks.size()));

        if (cancelFlag && *cancelFlag) {
            emit finished();
            return;
        }

        // Step 2: Optional Stockfish analysis (decoupled from PGN generation)
        StockfishAnalysisHelper sfHelper(settings, gameData, cancelFlag);
        connect(&sfHelper, &StockfishAnalysisHelper::logMessage, this, &VideoProcessorWorker::logMessage, Qt::DirectConnection);
        connect(&sfHelper, &StockfishAnalysisHelper::progressUpdated, this, &VideoProcessorWorker::progressUpdated, Qt::DirectConnection);
        
        if (runStockfishAnalysis) {
            emit logMessage("Stage 2/4: running Stockfish analysis.");
            if (!sfHelper.runAnalysis()) {
                emit finished();
                return;
            }
        } else {
            emit logMessage("Stage 2/4: Stockfish analysis not requested.");
        }
        
        const auto& sfResults = sfHelper.getResults();
        const auto& mainLineStockfishResults = sfResults.mainLineResults;
        const auto& videoStockfishResults = sfResults.videoResults;
        const auto& fenAnalysisCache = sfResults.fenCache;
        const auto& move_annotations = sfResults.moveAnnotations;
        int white_estimated_elo = sfResults.whiteEstimatedElo;
        int black_estimated_elo = sfResults.blackEstimatedElo;
        double white_acpl = sfResults.whiteAcpl;
        double black_acpl = sfResults.blackAcpl;

        emit logMessage("Stage 3/4: resolving opening metadata.");
        lichessHelper.waitAndProcess(gameData, cancelFlag);
        const auto& lichessResults = lichessHelper.getResults();
        std::string final_eco = lichessResults.finalEco;
        std::string final_opening_name = lichessResults.finalOpeningName;
        const auto& video_opening_names = lichessResults.videoOpeningNames;

        // Step 4: Export results
        emit logMessage("Stage 4/4: exporting requested outputs.");
        if (!exportHelper.exportAll(settings, gameData, sfResults, lichessResults, *extractor.get_board_geometry(), cancelFlag)) {
            if (cancelFlag && *cancelFlag) {
                emit finished();
            }
            return;
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
