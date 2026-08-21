#include "LichessSyncHelper.h"
#include "VideoProcessorWorker.h" // For GameData
#include "ChessVideoExtractor.h"
#include <thread>
#include <chrono>
#include <stdexcept>
#include <QSettings>

namespace cta {

LichessSyncHelper::LichessSyncHelper(QObject* parent) : QObject(parent) {
    // If the user configures the token in the GUI, it securely stores in the OS AppData/Registry.
    QString guiToken = QSettings().value("lichessToken", "").toString().trimmed();
    if (!guiToken.isEmpty()) {
        opening_fetcher_.set_api_token(guiToken.toStdString());
    }

    std::string error_msg;
    if (!opening_fetcher_.test_connection(error_msg)) {
        throw std::runtime_error(error_msg);
    }
}

std::function<void(const std::string&)> LichessSyncHelper::getFenCallback() {
    return [this](const std::string& fen) {
        opening_fetcher_.enqueue_fen(fen);
    };
}

void LichessSyncHelper::waitAndProcess(const GameData& gameData, std::atomic<bool>* cancelFlag) {
    emit logMessage("Waiting for background Lichess opening data to finish...");
    results_ = {};

    std::atomic<bool> fetcher_done{false};
    std::thread monitor_thread([this, &fetcher_done, cancelFlag, &gameData]() {
        int elapsed_ms = 0;
        size_t fen_idx = 0;
        int last_reported_s = 0;

        while (true) {
            // Look for newly completed queries in the OpeningFetcher cache
            while (fen_idx < gameData.video_fens.size()) {
                LichessOpening info = opening_fetcher_.get_opening(gameData.video_fens[fen_idx]);
                if (info.total_games == -1) break; // Not fetched yet

                if (info.game_metadata.found) {
                    results_.gameMetadata = info.game_metadata;
                }

                QString move_str = (fen_idx < gameData.video_moves.size()) ? QString::fromStdString(gameData.video_moves[fen_idx]) : QString("FEN %1").arg(fen_idx);

                if (info.api_success || info.total_games > 0) {
                    emit logMessage(QString("Lichess checked %1 -> %2 hits").arg(move_str).arg(info.total_games));
                    if (info.total_games == 0 && fen_idx > 0) {
                        LichessOpening prev_info = opening_fetcher_.get_opening(gameData.video_fens[fen_idx - 1]);
                        if (prev_info.total_games > 0 && !prev_info.top_games.empty()) {
                            emit logMessage("0 hits reached. Games from previous position:");
                            for (const auto& game : prev_info.top_games) {
                                emit logMessage(QString("  - %1").arg(QString::fromStdString(game)));
                            }
                        }
                    } else if (info.total_games == 1 && !info.top_games.empty()) {
                        emit logMessage(QString("Found matching game: %1").arg(QString::fromStdString(info.top_games[0])));
                    }
                } else {
                    QString detail = info.error.empty() ? "API request failed" : QString::fromStdString(info.error);
                    emit logMessage(QString("Lichess checked %1 -> %2").arg(move_str, detail));
                }

                fen_idx++;
                if (info.total_games <= 1 && info.api_success) break; // Unique game reached, fetcher stopped
            }

            if (fetcher_done.load() || (cancelFlag && cancelFlag->load())) {
                break;
            }

            // Sleep in small increments to respond quickly to fetcher_done or cancellation
            for (int i = 0; i < 5 && !fetcher_done.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                elapsed_ms += 100;
                if (cancelFlag && cancelFlag->load()) break;
            }

            int elapsed_s = elapsed_ms / 1000;
            if (elapsed_s >= last_reported_s + 5 && !fetcher_done.load()) {
                QString waiting_on = (fen_idx < gameData.video_moves.size()) ? QString::fromStdString(gameData.video_moves[fen_idx]) : "completion";
                emit logMessage(QString("Still fetching Lichess opening data... (%1s elapsed, waiting on %2)").arg(elapsed_s).arg(waiting_on));
                last_reported_s = elapsed_s;
            }
        }
    });

    opening_fetcher_.wait_until_done();
    fetcher_done = true;
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }

    // Game identity must follow the verified main line. The video timeline
    // intentionally includes analysis branches and REVERT markers, which are
    // useful for overlays but cannot be replayed as one Lichess game.
    opening_fetcher_.resolve_game_metadata(gameData.fens, gameData.moves);

    results_.gameMetadata = {};
    for (const std::string& fen : gameData.fens) {
        const LichessOpening info = opening_fetcher_.get_opening(fen);
        if (info.game_metadata.found) {
            results_.gameMetadata = info.game_metadata;
            break;
        }
    }

    results_.videoOpeningNames.assign(gameData.video_fens.size(), "");
    for (size_t i = 0; i < gameData.video_fens.size(); ++i) {
        LichessOpening info = opening_fetcher_.get_opening(gameData.video_fens[i]);
        if (info.found) {
            results_.finalEco = info.eco;
            results_.finalOpeningName = info.name;
        }
        results_.videoOpeningNames[i] = results_.finalEco.empty() ? "" : (results_.finalEco + " " + results_.finalOpeningName);
    }
}

} // namespace cta
