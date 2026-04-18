#include "MainWindow.h"
#include "ToggleSwitch.h"
#include "VideoProcessorWorker.h"
#include "SettingsDialog.h"

#include <QSettings>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRadioButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDirIterator>
#include <QPushButton>
#include <QListWidgetItem>

namespace aa {

ProcessingSettings MainWindow::gatherSettings() const {
    ProcessingSettings s;
    QString currentVideo = property("currentVideo").toString();
    if (currentVideo.isEmpty()) {
        if (auto* item = nextQueuedItem()) {
            currentVideo = item->data(Qt::UserRole).toString();
        }
    }
    s.videoPath = currentVideo.trimmed();
    
    // Robust path resolution for board asset
    QString boardOverride = property("headlessBoardAssetOverride").toString();
    if (!boardOverride.isEmpty()) {
        s.boardAssetPath = boardOverride;
    } else {
        QString assetPath = "assets/board/board.png";
        if (QFileInfo::exists(assetPath)) {
            s.boardAssetPath = assetPath;
        } else {
            QString buildFallback = QDir(QCoreApplication::applicationDirPath()).filePath("../../" + assetPath);
            if (QFileInfo::exists(buildFallback)) {
                s.boardAssetPath = buildFallback;
            } else {
                s.boardAssetPath = assetPath; // Fallback
            }
        }
    }
    
    // Robust path resolution for assets directory
    QString assetsDir = "assets";
    if (!QDir(assetsDir).exists()) {
        assetsDir = QDir(QCoreApplication::applicationDirPath()).filePath("../../assets");
    }
    s.assetsPath = assetsDir;
    
    QString baseDir = "output";
    QSettings qs(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat);
    bool sameAsSource = qs.value("outSameAsSource", true).toBool();
    QString customDir = qs.value("outCustomDir", "").toString();
    
    if (sameAsSource) {
        if (!s.videoPath.isEmpty()) {
            baseDir = QFileInfo(s.videoPath).absolutePath();
        }
    } else if (!customDir.isEmpty()) {
        baseDir = customDir;
    }
    
    QString outOverride = property("headlessOutputOverride").toString();
    if (!outOverride.isEmpty()) {
        QFileInfo outInfo(outOverride);
        if (outInfo.isDir() || outOverride.endsWith("/") || outOverride.endsWith("\\")) {
            baseDir = outOverride;
        } else {
            s.outputPath = outOverride;
        }
    }
    
    if (s.outputPath.isEmpty()) {
        QString baseName = "analysis";
        if (!s.videoPath.isEmpty()) {
            baseName = QFileInfo(s.videoPath).completeBaseName();
        }
        s.outputPath = QDir(baseDir).filePath(baseName + ".pgn");
        QDir().mkpath(baseDir); // Ensure output directory exists
    }
    
    settingsDialog_->populateSettings(s);
    return s;
}

} // namespace aa
