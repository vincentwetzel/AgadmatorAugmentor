#pragma once

#include <QString>
#include "VideoOverlayConfig.h"

namespace cta {

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
    int stockfishTime = 0;
    int stockfishNodes = 0;
    int stockfishAnalysisDepth = 5;
    QString stockfishPath;
    QString redBoardAssetPath;
    int debugLevel = 0; // 0: None, 1: Moves, 2: Full
    int memoryLimitMB = 0; // 0 = Unlimited
    VideoOverlayConfig overlayConfig;
};

} // namespace cta
