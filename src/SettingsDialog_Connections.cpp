#include "SettingsDialog.h"
#include "ToggleSwitch.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

namespace {
QString autoFindStockfish() {
    QString foundPath;
    QString appDir = QCoreApplication::applicationDirPath();
    QString execName = "stockfish";
#ifdef _WIN32
    execName = "stockfish.exe";
#endif
    const QStringList exePatterns = {
#ifdef _WIN32
        "stockfish.exe",
        "stockfish-*.exe",
#else
        "stockfish",
        "stockfish-*",
#endif
    };

    auto findMatchingExecutable = [&exePatterns](const QString& directoryPath) -> QString {
        QDir dir(directoryPath);
        if (!dir.exists()) return {};
        for (const QString& pattern : exePatterns) {
            const QFileInfoList files = dir.entryInfoList(QStringList() << pattern, QDir::Files | QDir::Executable | QDir::NoSymLinks);
            if (!files.isEmpty()) return files.first().absoluteFilePath();
        }
        return {};
    };

    QStringList candidatePaths = {
        appDir + "/stockfish/" + execName, appDir + "/../stockfish/" + execName,
        appDir + "/../../stockfish/" + execName, appDir + "/" + execName
    };
    for (const QString& p : candidatePaths) {
        if (QFileInfo::exists(p)) return QFileInfo(p).absoluteFilePath();
    }
    
    const QStringList candidateDirs = {
        appDir, appDir + "/stockfish", appDir + "/../stockfish", appDir + "/../../stockfish",
        "C:/stockfish", "C:/stockfish/stockfish", "C:/stockfish-windows-x86-64-avx2", "C:/stockfish-windows-x86-64-avx2/stockfish"
    };
    for (const QString& dirPath : candidateDirs) {
        foundPath = findMatchingExecutable(dirPath);
        if (!foundPath.isEmpty()) return foundPath;
    }
    
    QStringList baseDirs = { QDir::rootPath(), "C:/", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) };
    for (const QString& base : baseDirs) {
        QDir dir(base);
        if (!dir.exists()) continue;
        QFileInfoList subdirs = dir.entryInfoList(QStringList() << "*stockfish*", QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& sInfo : subdirs) {
            QDirIterator it(sInfo.absoluteFilePath(), exePatterns, QDir::Files | QDir::Executable, QDirIterator::Subdirectories);
            if (it.hasNext()) return it.next();
        }
    }
    return {};
}
} // namespace

namespace cta {

void SettingsDialog::connectAutoSaveSignals() {
    connect(ui.pgnExportToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "PGN export enabled" : "PGN export disabled");
        ui.pgnAnalysisToggle->setEnabled(checked);
        ui.pgnAnnotationsToggle->setEnabled(checked && ui.pgnAnalysisToggle->isChecked());
        if (!checked) {
            ui.pgnAnalysisToggle->setChecked(false);
            ui.pgnAnnotationsToggle->setChecked(false);
        }
        saveSettings();
    });
    connect(ui.subtitlesToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Move subtitles enabled" : "Move subtitles disabled");
        if (checked && !ui.analysisVideoToggle->isChecked()) {
            ui.analysisVideoToggle->setChecked(true);
        }
        saveSettings();
    });
    connect(ui.externalSubtitlesToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "External SRT export enabled" : "External SRT export disabled");
        saveSettings();
    });
    connect(ui.analysisVideoToggle, &ToggleSwitch::toggled, this, [this](bool checked) { 
        emit logMessage(checked ? "Analysis Video generation enabled" : "Analysis Video generation disabled");
        if (!checked && ui.subtitlesToggle->isChecked()) {
            ui.subtitlesToggle->setChecked(false);
        }
        ui.videoAnnotationsToggle->setEnabled(checked);
        if (!checked) {
            ui.videoAnnotationsToggle->setChecked(false);
        }
        saveSettings(); 
    });
    connect(ui.removeOriginalToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Original video will be deleted after processing" : "Original video will be kept after processing");
        saveSettings();
    });
    connect(ui.threadComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.videoExportThreadsComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.multiPvComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    
    connect(ui.stockfishPathBtn, &QPushButton::clicked, this, [this]() {
        QSettings settings;
        QString lastDir = settings.value("lastStockfishDir", QDir::homePath()).toString();
        QString filter = "All Files (*)";
#ifdef _WIN32
        filter = "Executables (*.exe);;All Files (*)";
#endif
        QString fileName = QFileDialog::getOpenFileName(this, "Select Stockfish Executable", lastDir, filter);
        if (!fileName.isEmpty()) {
            ui.stockfishPathEdit->setText(fileName);
            settings.setValue("lastStockfishDir", QFileInfo(fileName).absolutePath());
            saveSettings();
        }
    });
    
    connect(ui.stockfishSearchBtn, &QPushButton::clicked, this, [this]() {
        emit logMessage("Searching for Stockfish executable...");
        QCoreApplication::processEvents();
        
        QString foundPath = autoFindStockfish();
        
        if (!foundPath.isEmpty()) {
            ui.stockfishPathEdit->setText(QDir::toNativeSeparators(foundPath));
            emit logMessage("Found Stockfish at: " + QDir::toNativeSeparators(foundPath));
            saveSettings();
        } else {
            emit logMessage("Could not automatically find Stockfish. Please browse manually.");
        }
    });

    connect(ui.depthComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.timeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.nodesComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.analysisDepthComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.stockfishPathEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    
    connect(ui.themeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        saveSettings();
        emit logMessage("Theme changed to: " + ui.themeComboBox->currentText());
        emit themeChanged();
    });
    
    connect(ui.pgnAnalysisToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        if (checked && !ui.pgnExportToggle->isChecked()) {
            ui.pgnExportToggle->setChecked(true);
        }
        ui.pgnAnnotationsToggle->setEnabled(checked);
        if (!checked && ui.pgnAnnotationsToggle->isChecked()) {
            ui.pgnAnnotationsToggle->setChecked(false);
        }
        saveSettings();
    });
    connect(ui.pgnAnnotationsToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        if (checked) ui.pgnAnalysisToggle->setChecked(true);
        saveSettings();
    });
    connect(ui.videoAnnotationsToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Video move quality labels enabled" : "Video move quality labels disabled");
        saveSettings();
    });
    
    connect(ui.videoCodecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const QString vCodec = ui.videoCodecComboBox->currentData().toString();
        const QString currentAudio = ui.audioCodecComboBox->currentData().toString();
        QString currentExt = ui.extensionComboBox->currentText();
        ui.audioCodecComboBox->blockSignals(true); ui.extensionComboBox->blockSignals(true);
        ui.audioCodecComboBox->clear(); ui.extensionComboBox->clear();
        if (vCodec == "libvpx-vp9") {
            ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
            ui.audioCodecComboBox->addItem("Opus audio (recommended for WebM)", "libopus");
            ui.extensionComboBox->addItems({".webm", ".mkv"});
        } else {
            ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
            ui.audioCodecComboBox->addItem("AAC audio (standard)", "aac");
            ui.extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
        }
        int aIdx = ui.audioCodecComboBox->findData(currentAudio);
        if (aIdx >= 0) ui.audioCodecComboBox->setCurrentIndex(aIdx);
        int eIdx = ui.extensionComboBox->findText(currentExt);
        if (eIdx >= 0) ui.extensionComboBox->setCurrentIndex(eIdx);
        ui.audioCodecComboBox->blockSignals(false); ui.extensionComboBox->blockSignals(false);
        saveSettings();
    });
    
    connect(ui.audioCodecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.extensionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.resolutionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.qualityComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(ui.debugLevelComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
}
} // namespace cta
