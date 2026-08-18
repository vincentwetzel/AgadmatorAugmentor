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
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace cta {

namespace {
    const qint64 MAX_LOG_SIZE = 5 * 1024 * 1024; // 5 MB per log file
    std::unique_ptr<QFile> g_logFile;
    std::unique_ptr<QFile> g_jobLogFile;
    QMutex g_logMutex;

    QString logDirectoryPath() {
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .absoluteFilePath("logs");
    }

    int retainedLogCount() {
        QSettings settings;
        return std::max(1, settings.value("logRetentionCount", 10).toInt());
    }

    void writeLogLine(QFile* file, const QString& line) {
        if (file == nullptr || !file->isOpen()) return;
        file->write(line.toUtf8());
        file->flush();
    }

    void trimLogs(QDir& directory, const QStringList& filters, int retainCount) {
        QFileInfoList fileList = directory.entryInfoList(
            filters, QDir::Files | QDir::NoSymLinks, QDir::Time | QDir::Reversed);
        while (fileList.size() > retainCount) {
            (void)QFile::remove(fileList.takeFirst().absoluteFilePath());
        }
    }
}

void Logger::cycleLogsIfNeeded() {
    QString logDirStr = logDirectoryPath();
    QDir logDir(logDirStr);
    if (!logDir.exists()) {
        logDir.mkpath(".");
    }

    QSettings settings;
    const int retainCount = retainedLogCount();
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

    trimLogs(logDir, QStringList() << "log_*.txt" << "log_*.zip", retainCount - 1);
}

void Logger::cycleJobLogsIfNeeded() {
    QDir jobDir(QDir(logDirectoryPath()).absoluteFilePath("jobs"));
    if (!jobDir.exists()) {
        jobDir.mkpath(".");
    }
    // Leave room for the job log that is about to be created.
    trimLogs(jobDir, QStringList() << "job_*.txt", retainedLogCount() - 1);
}

void Logger::setup() {
    cycleLogsIfNeeded();
    QString logDirStr = logDirectoryPath();
    QString currentDateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    QString logFilePath = QDir(logDirStr).absoluteFilePath(QString("log_%1.txt").arg(currentDateTime));

    g_logFile = std::make_unique<QFile>(logFilePath);
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
                writeLogLine(g_logFile.get(), fileLogLine);
                writeLogLine(g_jobLogFile.get(), fileLogLine);

                // Automatically cycle the active log file if it exceeds the maximum size boundary
                if (g_logFile->size() > MAX_LOG_SIZE) {
                    g_logFile->close();
                    g_logFile.reset();

                    cycleLogsIfNeeded();
                    QString logDir = logDirectoryPath();
                    QString newDateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
                    g_logFile = std::make_unique<QFile>(QDir(logDir).absoluteFilePath(QString("log_%1.txt").arg(newDateTime)));
                    (void)g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
                }
            }

            if (type == QtFatalMsg) {
                abort();
            }
        });
    }
}

QString Logger::beginJob(const QString& videoPath) {
    QMutexLocker lock(&g_logMutex);
    if (g_jobLogFile && g_jobLogFile->isOpen()) {
        writeLogLine(g_jobLogFile.get(), QString("[%1] Job ended: superseded by a new job\n")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")));
        g_jobLogFile->close();
    }

    cycleJobLogsIfNeeded();
    const QDir jobDir(QDir(logDirectoryPath()).absoluteFilePath("jobs"));
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss_zzz");
    const QString baseName = QFileInfo(videoPath).completeBaseName().replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    const QString path = jobDir.absoluteFilePath(
        QString("job_%1_%2.txt").arg(timestamp, baseName.isEmpty() ? "video" : baseName));
    g_jobLogFile = std::make_unique<QFile>(path);
    if (!g_jobLogFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        g_jobLogFile.reset();
        return {};
    }

    writeLogLine(g_jobLogFile.get(), QString("[%1] ChessTube Analyzer job started\n[%1] Input video: %2\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), videoPath));
    return path;
}

void Logger::endJob(const QString& outcome) {
    QMutexLocker lock(&g_logMutex);
    if (!g_jobLogFile || !g_jobLogFile->isOpen()) return;
    writeLogLine(g_jobLogFile.get(), QString("[%1] Job ended: %2\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), outcome));
    g_jobLogFile->close();
    g_jobLogFile.reset();
}

QString Logger::activeJobPath() {
    QMutexLocker lock(&g_logMutex);
    return g_jobLogFile ? g_jobLogFile->fileName() : QString();
}

void Logger::cleanup() {
    QMutexLocker lock(&g_logMutex);
    if (g_logFile) {
        g_logFile->close();
        g_logFile.reset();
    }
    if (g_jobLogFile) {
        g_jobLogFile->close();
        g_jobLogFile.reset();
    }
}

} // namespace cta
