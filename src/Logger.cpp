#include "Logger.h"

#include <QFile>
#include <QMutex>
#include <QSettings>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <QDateTime>
#include <QFileInfoList>
#include <QMessageLogContext>
#include <cstdio>
#include <cstdlib>

namespace cta {

namespace {
    const qint64 MAX_LOG_SIZE = 5 * 1024 * 1024; // 5 MB per log file
    QFile* g_logFile = nullptr;
    QMutex g_logMutex;
}

void Logger::cycleLogsIfNeeded() {
    QString logDirStr = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath("logs");
    QDir logDir(logDirStr);
    if (!logDir.exists()) {
        logDir.mkpath(".");
    }

    QSettings settings;
    int retainCount = settings.value("logRetentionCount", 10).toInt();
    if (retainCount < 1) retainCount = 1;
    bool compressLogs = settings.value("compressOldLogs", false).toBool();

    if (compressLogs) {
        QFileInfoList txtFiles = logDir.entryInfoList(QStringList() << "log_*.txt", QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo& fi : txtFiles) {
            QString zipName = fi.completeBaseName() + ".zip";
            QProcess process;
            process.setWorkingDirectory(logDir.absolutePath());
            process.start("tar", QStringList() << "-a" << "-c" << "-f" << zipName << fi.fileName());
            if (process.waitForFinished() && process.exitCode() == 0) {
                (void)QFile::remove(fi.absoluteFilePath());
            }
        }
    }

    QFileInfoList fileList = logDir.entryInfoList(QStringList() << "log_*.txt" << "log_*.zip", QDir::Files | QDir::NoSymLinks, QDir::Name);
    while (fileList.size() >= retainCount) {
        (void)QFile::remove(fileList.first().absoluteFilePath());
        fileList.removeFirst();
    }
}

void Logger::setup() {
    cycleLogsIfNeeded();
    QString logDirStr = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath("logs");
    QString currentDateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    QString logFilePath = QDir(logDirStr).absoluteFilePath(QString("log_%1.txt").arg(currentDateTime));

    g_logFile = new QFile(logFilePath);
    if (g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& msg) {
            Q_UNUSED(context);
            QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
            QString consoleLogLine = msg + "\n";
            QString fileLogLine = QString("[%1] %2\n").arg(timestamp, msg);

            if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
                fprintf(stderr, "%s", consoleLogLine.toLocal8Bit().constData());
                fflush(stderr);
            } else {
                fprintf(stdout, "%s", consoleLogLine.toLocal8Bit().constData());
                fflush(stdout);
            }

            QMutexLocker lock(&g_logMutex);
            if (g_logFile && g_logFile->isOpen()) {
                g_logFile->write(fileLogLine.toUtf8());
                g_logFile->flush();

                // Automatically cycle the active log file if it exceeds the maximum size boundary
                if (g_logFile->size() > MAX_LOG_SIZE) {
                    g_logFile->close();
                    delete g_logFile;
                    g_logFile = nullptr;

                    cycleLogsIfNeeded();
                    QString logDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath("logs");
                    QString newDateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
                    g_logFile = new QFile(QDir(logDir).absoluteFilePath(QString("log_%1.txt").arg(newDateTime)));
                    (void)g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
                }
            }

            if (type == QtFatalMsg) {
                abort();
            }
        });
    }
}

void Logger::cleanup() {
    QMutexLocker lock(&g_logMutex);
    if (g_logFile) {
        g_logFile->close();
        delete g_logFile;
        g_logFile = nullptr;
    }
}

} // namespace cta