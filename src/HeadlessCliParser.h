#pragma once

#include <QString>

namespace cta {

struct CliOptions {
    bool isHeadless = false;
    bool showHelp = false;
    bool showVersion = false;
    bool parseError = false;
    QString errorMessage;
    QString helpText;

    QString videoPath;
    QString boardAsset;
    QString output;
    QString debugLevelStr = "MOVES";

    int multiPv = -1;
    int depth = -1;
    int time = -1;
    int nodes = -1;
    int analysisDepth = -1;
    int threads = -1;
    int memoryLimit = -1;

    int pgnOverride = -1;
    int analysisVideoOverride = -1;
    int moveLabelsOverride = -1;
};

class HeadlessCliParser {
public:
    static CliOptions parse();
};

} // namespace cta