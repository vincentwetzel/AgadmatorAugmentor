#pragma once

#include "ProcessingSettings.h"
#include "StockfishAnalysisHelper.h"
#include "LichessSyncHelper.h"
#include "BoardLocalizer.h"
#include "ChessVideoExtractor.h"
#include <QObject>
#include <QString>
#include <atomic>

namespace cta {

class VideoExportHelper : public QObject {
    Q_OBJECT
public:
    explicit VideoExportHelper(QObject* parent = nullptr);

    void verifyEnvironment(const ProcessingSettings& settings, bool runStockfishAnalysis);

    bool exportAll(const ProcessingSettings& settings,
                   const GameData& gameData,
                   const StockfishAnalysisResults& sfResults,
                   const LichessSyncResults& lichessResults,
                   const BoardGeometry& geo,
                   std::atomic<bool>* cancelFlag);

signals:
    void logMessage(const QString& message);
    void progressUpdated(int percentage);
    void error(const QString& errorMessage);

private:
    bool generatePgn(const ProcessingSettings& settings, const GameData& gameData, const StockfishAnalysisResults& sfResults, const LichessSyncResults& lichessResults);
    bool generateSubtitles(const ProcessingSettings& settings, const GameData& gameData, const QString& subtitlePath);
    bool generateVideo(const ProcessingSettings& settings, const GameData& gameData, const StockfishAnalysisResults& sfResults, const LichessSyncResults& lichessResults, const BoardGeometry& geo, const QString& outPath, const QString& subtitlePath, std::atomic<bool>* cancelFlag);
};

} // namespace cta