// Extracted from cpp directory
#include "VideoProcessorWorker.h"
#include "ChessVideoExtractor.h"
#include "AnalysisVideoGenerator.h"
#include "PgnWriter.h"
#include "StockfishAnalyzer.h"
#include "BoardLocalizer.h"

#include <exception>
#include <fstream>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <map>

namespace aa {

namespace { // Anonymous namespace for helper function
    std::string format_clock_string(std::string clockStr) {
        // Standard PGN clock format requires hours: [%clk h:mm:ss]
        if (clockStr.empty()) {
            return "0:00:00"; // Fallback for blank or malformed clocks
        } else if (std::count(clockStr.begin(), clockStr.end(), ':') == 0) {
            // If there are no colons but there is a decimal (e.g. "14.5")
            if (clockStr.find('.') != std::string::npos) {
                if (clockStr.find('.') == 1) {
                    return "0:00:0" + clockStr; // e.g., "9.5" -> "0:00:09.5"
                } else {
                    return "0:00:" + clockStr;  // e.g., "14.5" -> "0:00:14.5"
                }
            } else {
                return "0:00:00"; // Fallback
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
}

void VideoProcessorWorker::process(const ProcessingSettings& settings, std::atomic<bool>* cancelFlag) {
    try {
        emit logMessage("Initializing extractor for: " + settings.videoPath);

        ChessVideoExtractor extractor(
            settings.boardAssetPath.toStdString(),
            settings.redBoardAssetPath.toStdString(),
            static_cast<DebugLevel>(settings.debugLevel)
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
        std::vector<StockfishResult> stockfishResults;
        if ((settings.enableStockfish || settings.generateAnalysisVideo) && !gameData.fens.empty()) {
            emit logMessage("Starting Stockfish analysis (MultiPV=" + QString::number(settings.multiPv) + ")...");
            
            StockfishAnalyzer analyzer(settings.multiPv, settings.stockfishPath.toStdString());
            analyzer.set_progress_callback([this, cancelFlag](int current, int total) {
                QString msg = QString("Analyzing position %1 of %2...").arg(current).arg(total);
                emit logMessage(msg);
            });
            stockfishResults = analyzer.analyze_positions(gameData.fens, settings.stockfishDepth, cancelFlag);
            if (cancelFlag && *cancelFlag) {
                emit finished();
                return;
            }
            
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
                pgn.add_ply(gameData.moves[i], format_clock_string(clockStr));

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
                            pgn.add_ply(var_data.moves[j], format_clock_string(var_clock_str));
                        }
                        pgn.pop_variation();
                    }
                }
            }

            // Inject Stockfish analysis if available (decoupled from PGN generation)
            if (!stockfishResults.empty()) {
                pgn.add_stockfish_analysis(stockfishResults, settings.stockfishAnalysisDepth);
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
            
            try {
                AnalysisVideoGenerator generator(settings.assetsPath.toStdString());
                
                QFileInfo outInfo(settings.outputPath);
                QString analysisVideoPath = outInfo.absoluteDir().filePath(outInfo.completeBaseName() + "_analysis.mp4");

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
                    bool success = generator.generate_analysis_video(
                        settings.videoPath.toStdString(),
                        analysisVideoPath.toStdString(),
                        *geo,
                        gameData.fens,
                        gameData.timestamps,
                        stockfishResults,
                    15, // Hardcoded engine arrow thickness percentage
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