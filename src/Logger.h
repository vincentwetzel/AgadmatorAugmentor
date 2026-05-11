#pragma once

namespace cta {

class Logger {
public:
    static void setup();
    static void cleanup();

private:
    static void cycleLogsIfNeeded();
};

} // namespace cta