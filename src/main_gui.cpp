// Extracted from cpp directory
#include "MainWindow.h"
#include "TemplateManager.h"
#include "Logger.h"
#include "SysUtils.h"
#include "HeadlessCliParser.h"
#include <QApplication>
#include <QSettings>
#include <QTextStream>
#include <QDir>
#include <QSysInfo>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <exception>
#include <stdlib.h>
#include <stdio.h>
#endif

namespace {

#ifdef _WIN32
LONG WINAPI UnhandledExceptionFilter_(EXCEPTION_POINTERS* exception_info);
#endif

void configure_platform_exception_handler() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledExceptionFilter_);
#endif
}

// Global exception handler for Windows SEH exceptions
#ifdef _WIN32
LONG WINAPI UnhandledExceptionFilter_(EXCEPTION_POINTERS* ExceptionInfo) {
    MessageBoxA(nullptr,
        "A fatal error occurred in the application.",
        "ChessTube Analyzer - Fatal Error",
        MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

int main(int argc, char *argv[]) {
    configure_platform_exception_handler();

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName("ChessTubeAnalyzer");
    QApplication::setApplicationName("settings");
    QApplication::setApplicationDisplayName("ChessTube Analyzer");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QApplication::setApplicationVersion("0.3.0");

    cta::Logger::setup();

    QStringList launchArgs;
    for (int i = 0; i < argc; ++i) {
        launchArgs << argv[i];
    }
    qInfo() << "=== ChessTube Analyzer Launched ===";
    qInfo() << "Launch arguments:" << launchArgs.join(" ");

    QString sysInfo = QString("OS: %1 | CPU Arch: %2").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        sysInfo += QString(" | Total RAM: %1 MB").arg(memInfo.ullTotalPhys / (1024 * 1024));
    }
#endif
    qInfo() << "System Info:" << sysInfo;

    // Initialize the template manager to load/copy templates from/to AppData.
    // This makes them available for both GUI and headless mode.
    cta::TemplateManager::instance().initialize();

    cta::CliOptions opts = cta::HeadlessCliParser::parse();
    
    if (opts.parseError) {
        std::cerr << opts.errorMessage.toStdString() << "\n\n" << opts.helpText.toStdString();
        cta::Logger::cleanup();
        return 1;
    }

    if (opts.showVersion) {
        std::cout << "ChessTube Analyzer v0.3.0" << std::endl;
        cta::Logger::cleanup();
        return 0;
    }

    cta::MainWindow main_window;
    int result = 0;

    if (opts.isHeadless) {
        result = main_window.processHeadless(opts.videoPath, opts.pgnOverride, opts.analysisVideoOverride, opts.moveLabelsOverride, opts.multiPv, opts.threads, opts.depth, opts.time, opts.nodes, opts.analysisDepth, opts.debugLevelStr, opts.output, opts.boardAsset, opts.memoryLimit);
    } else {
        main_window.show();
        result = app.exec();
    }

    cta::Logger::cleanup();
    return result;
}
