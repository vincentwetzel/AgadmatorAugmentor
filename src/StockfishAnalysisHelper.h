#pragma once

#include "StockfishAnalyzer.h"
#include "ProcessingSettings.h"
#include "libchess/position.hpp"
#include "ChessVideoExtractor.h"
#include <QObject>
#include <atomic>
#include <map>
#include <vector>
#include <string>
#include <functional> // For std::function
#include <memory>     // For std::unique_ptr

namespace cta {

// Helper struct to encapsulate all Stockfish analysis results
struct StockfishAnalysisResults {
    std::vector<StockfishResult> mainLineResults;
    std::vector<StockfishResult> videoResults;
    std::map<std::string, StockfishResult> fenCache;
    std::vector<std::string> moveAnnotations; // Annotations for mainLineResults
    int whiteEstimatedElo = 0;
    int blackEstimatedElo = 0;
    double whiteAcpl = -1.0;
    double blackAcpl = -1.0;
};

class StockfishAnalysisHelper : public QObject {
    Q_OBJECT

public:
    StockfishAnalysisHelper(const ProcessingSettings& settings,
                            GameData& gameData, // Modifiable gameData to update videoStockfishResults
                            std::atomic<bool>* cancelFlag,
                            QObject* parent = nullptr);
    ~StockfishAnalysisHelper() override;

    // Runs the main Stockfish analysis and annotation process
    // Returns true on success, false if cancelled or error
    bool runAnalysis();

    const StockfishAnalysisResults& getResults() const { return results_; }

signals:
    void logMessage(const QString& message);
    void progressUpdated(int percentage);

private:
    const ProcessingSettings& settings_;
    GameData& gameData_;
    std::atomic<bool>* cancelFlag_;
    StockfishAnalysisResults results_;

    // Internal StockfishAnalyzer instance
    std::unique_ptr<StockfishAnalyzer> analyzer_;

    void processVideoMoveAnnotations();
    void processMainLineAnnotationsAndStats();
};

} // namespace cta