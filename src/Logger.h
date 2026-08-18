#pragma once

#include <QString>

namespace cta {

class Logger {
public:
    static void setup();
    static void cleanup();

    // Each video receives its own readable log in addition to the application
    // session log. The Qt message handler mirrors worker and UI progress into
    // this file until endJob() is called.
    static QString beginJob(const QString& videoPath);
    static void endJob(const QString& outcome);
    static QString activeJobPath();

private:
    static void cycleLogsIfNeeded();
    static void cycleJobLogsIfNeeded();
};

} // namespace cta
