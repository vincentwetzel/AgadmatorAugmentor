#include "VideoProcessorWorker.h"
#include "ChessVideoExtractor.h"

#include <exception>

namespace aa {

VideoProcessorWorker::VideoProcessorWorker(QObject* parent) : QObject(parent) {}

void VideoProcessorWorker::process(const QString& videoPath, const QString& boardAssetPath, const QString& outputPath) {
    emit logMessage("Initializing extractor for: " + videoPath);

    try {
        ChessVideoExtractor extractor(
            boardAssetPath.toStdString(),
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

        extractor.extract_moves_from_video(videoPath.toStdString(), outputPath.toStdString());

        emit finished();

    } catch (const std::exception& e) {
        emit error(QString::fromStdString(e.what()));
    } catch (...) {
        emit error("An unknown error occurred during video processing.");
    }
}

} // namespace aa