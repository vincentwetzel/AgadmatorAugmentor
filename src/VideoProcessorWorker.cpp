// Extracted from cpp directory
#include "VideoProcessorWorker.h"
#include "ChessVideoExtractor.h"
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"

#include <exception>
#include <fstream>
#include <QDir>
#include <QFileInfo>

namespace aa {

VideoProcessorWorker::VideoProcessorWorker(QObject* parent) : QObject(parent) {}

void VideoProcessorWorker::process(const ProcessingSettings& settings) {
    try {
        emit logMessage("Initializing extractor for: " + settings.videoPath);

        ChessVideoExtractor extractor(
            settings.boardAssetPath.toStdString(),
            "", // red_board_asset_path
            DebugLevel::None
        );

        extractor.set_progress_callback([this](int percent, const std::string& message) {
            if (percent >= 0) {
                emit progressUpdated(percent);
            }
            if (!message.empty()) {
                emit logMessage(QString::fromStdString(message));
            }
        });

        // Step 1: Extract moves from video
        std::string jsonOutputPath = settings.enableStockfish ? settings.outputPath.toStdString() : "";
        GameData gameData = extractor.extract_moves_from_video(
            settings.videoPath.toStdString(),
            jsonOutputPath
        );

        // Step 2: Optional Stockfish analysis (decoupled from PGN generation)
        std::vector<StockfishResult> stockfishResults;
        if (settings.enableStockfish && !gameData.fens.empty()) {
            emit logMessage("Starting Stockfish analysis (MultiPV=" + QString::number(settings.multiPv) + ")...");
            
            StockfishAnalyzer analyzer(settings.multiPv);
            stockfishResults = analyzer.analyze_positions(gameData.fens);
            
            emit logMessage("Stockfish analysis complete. Analyzed " + 
                          QString::number(stockfishResults.size()) + " positions.");
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

            // Add moves with clock info
            for (size_t i = 0; i < gameData.moves.size(); ++i) {
                std::string clockStr;
                if (i < gameData.clocks.size()) {
                    const auto& clk = gameData.clocks[i];
                    clockStr = clk.white_time + "/" + clk.black_time;
                }
                pgn.add_ply(gameData.moves[i], clockStr);
            }

            // Inject Stockfish analysis if available (decoupled from PGN generation)
            if (!stockfishResults.empty()) {
                pgn.add_stockfish_analysis(stockfishResults);
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