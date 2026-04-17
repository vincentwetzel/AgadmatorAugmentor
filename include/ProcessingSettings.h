#pragma once

#include <QString>

namespace aa {

struct ProcessingSettings {
    QString videoPath;
    QString outputPath;
    QString boardAssetPath;
    QString assetsPath;
    bool generatePgn = true;
    bool enableStockfish = false;
    bool generateAnalysisVideo = false;
    int multiPv = 3;
    int ffmpegThreads = 4;
    int stockfishDepth = 15;
    int stockfishAnalysisDepth = 5;
    QString stockfishPath;
    QString redBoardAssetPath;
    int debugLevel = 0; // 0: None, 1: Moves, 2: Full
};

} // namespace aa
