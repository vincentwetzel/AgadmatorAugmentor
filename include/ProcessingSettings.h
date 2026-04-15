#pragma once

#include <QString>

namespace aa {

struct ProcessingSettings {
    QString videoPath;
    QString boardAssetPath;
    QString outputPath;
    bool generatePgn = true;
    bool enableStockfish = false;
    int multiPv = 3;
    int ffmpegThreads = 4;
    int stockfishDepth = 15;
    int stockfishTime = 1000;
};

} // namespace aa

// Make ProcessingSettings available to Qt meta-object system
Q_DECLARE_METATYPE(aa::ProcessingSettings)
