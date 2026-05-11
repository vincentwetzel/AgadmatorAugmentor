#include "SettingsDialog.h"
#include "ToggleSwitch.h"
#include "ThemeManager.h"
#include "SysUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QComboBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDirIterator>
#include <algorithm>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QTimer>
#include <QCheckBox>
#include <QSysInfo>
#include <thread>

namespace {
QWidget* createToggleRow(const QString& label, const QString& tooltip, ToggleSwitch*& outToggle, bool checked = false) {
    auto* rowWidget = new QWidget();
    auto* layout = new QHBoxLayout(rowWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    
    auto* textLabel = new QLabel(label);
    textLabel->setToolTip(tooltip);
    textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(textLabel);
    
    outToggle = new ToggleSwitch("", checked);
    outToggle->setToolTip(tooltip);
    layout->addWidget(outToggle);
    layout->addStretch();
    
    rowWidget->setLayout(layout);
    return rowWidget;
}

QLabel* createSettingsLabel(const QString& text, const QString& tooltip) {
    auto* label = new QLabel(text);
    label->setToolTip(tooltip);
    return label;
}

QLabel* createHelpText(const QString& text, const QString& tooltip) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setToolTip(tooltip);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return label;
}

QComboBox* createComboBoxRow(QBoxLayout* parentLayout, const QString& labelText, const QString& labelTooltip,
                             const QString& objName, const QString& comboTooltip, QWidget* extraWidget = nullptr) {
    auto* layout = new QHBoxLayout();
    layout->addWidget(createSettingsLabel(labelText, labelTooltip));
    auto* combo = new QComboBox();
    if (!objName.isEmpty()) combo->setObjectName(objName);
    combo->setProperty("class", "dropdown");
    if (!comboTooltip.isEmpty()) combo->setToolTip(comboTooltip);
    layout->addWidget(combo);
    if (extraWidget) {
        layout->addWidget(extraWidget);
    }
    layout->addStretch();
    parentLayout->addLayout(layout);
    return combo;
}

void setThreadComboValue(QComboBox* comboBox, int threads) {
    if (!comboBox) {
        return;
    }

    const int clampedThreads = std::clamp(threads, 1, cta::SysUtils::max_hardware_thread_count());
    const int index = comboBox->findData(clampedThreads);
    comboBox->setCurrentIndex(index >= 0 ? index : comboBox->count() - 1);
}

QString canonicalVideoCodec(const QString& savedValue) {
    if (savedValue.contains("h264_nvenc") || savedValue.contains("NVIDIA GPU H.264")) return "h264_nvenc";
    if (savedValue.contains("libx265") || savedValue.contains("HEVC CPU")) return "libx265";
    if (savedValue.contains("hevc_nvenc") || savedValue.contains("NVIDIA GPU HEVC")) return "hevc_nvenc";
    if (savedValue.contains("libvpx-vp9") || savedValue.contains("VP9")) return "libvpx-vp9";
    return "libx264";
}

QString canonicalAudioCodec(const QString& savedValue) {
    if (savedValue.contains("libopus")) return "libopus";
    if (savedValue.contains("aac", Qt::CaseInsensitive) || savedValue.contains("AAC")) return "aac";
    return "copy";
}

QString canonicalResolution(const QString& savedValue) {
    if (savedValue.contains("3840") || savedValue.contains("4K")) return "4K";
    if (savedValue.contains("1920") || savedValue.contains("1080")) return "1080p";
    if (savedValue.contains("1280") || savedValue.contains("720")) return "720p";
    return "Source Resolution";
}

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

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Settings");
    resize(760, 560);
    setupUi();
}

void SettingsDialog::setupUi() {
    auto* dialogLayout = new QVBoxLayout(this);
    auto* tabWidget = new QTabWidget();
    tabWidget->setToolTip("Navigate between different settings categories");
    dialogLayout->addWidget(tabWidget);

    // === Tab 1: General ===
    auto* generalTab = new QWidget();
    auto* generalLayout = new QVBoxLayout(generalTab);

    auto* inputDirGroup = new QGroupBox("Default Video Folder");
    inputDirGroup->setToolTip("Choose the default folder that opens when you click Add Video(s).");
    auto* inputDirLayout = new QHBoxLayout(inputDirGroup);

    ui.defaultVideoDirEdit = new QLineEdit();
    ui.defaultVideoDirEdit->setObjectName("defaultVideoDirEdit");
    ui.defaultVideoDirEdit->setToolTip("Leave blank to use the system home folder.");
    inputDirLayout->addWidget(ui.defaultVideoDirEdit);

    auto* defaultVideoDirBtn = new QPushButton("Browse...");
    defaultVideoDirBtn->setToolTip("Choose the default folder for opening videos.");
    inputDirLayout->addWidget(defaultVideoDirBtn);

    connect(ui.defaultVideoDirEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    connect(defaultVideoDirBtn, &QPushButton::clicked, this, [this]() {
        QString currentDir = ui.defaultVideoDirEdit->text();
        if (currentDir.isEmpty()) currentDir = QDir::homePath();
        QString dir = QFileDialog::getExistingDirectory(this, "Select Default Video Folder", currentDir);
        if (!dir.isEmpty()) {
            ui.defaultVideoDirEdit->setText(dir);
        }
    });

    generalLayout->addWidget(inputDirGroup);

    auto* outputDirGroup = new QGroupBox("Where to Save Results");
    outputDirGroup->setToolTip("Choose where PGN files and analysis videos will be saved.");
    auto* outputDirLayout = new QVBoxLayout(outputDirGroup);
    outputDirLayout->addWidget(createHelpText(
        "Choose a default folder for files created after each video finishes.",
        "Explains how ChessTube Analyzer chooses the output folder."
    ));

    ui.sameAsSourceRadio = new QRadioButton("Next to each source video");
    ui.sameAsSourceRadio->setObjectName("sameAsSourceRadio");
    ui.sameAsSourceRadio->setChecked(true);
    ui.sameAsSourceRadio->setToolTip("Save each video's output files in the same folder as that input video.");
    outputDirLayout->addWidget(ui.sameAsSourceRadio);

    auto* customDirHLayout = new QHBoxLayout();
    ui.customDirRadio = new QRadioButton("One folder for all results:");
    ui.customDirRadio->setObjectName("customDirRadio");
    ui.customDirRadio->setToolTip("Save every output file to the folder you choose here.");
    customDirHLayout->addWidget(ui.customDirRadio);

    ui.customDirEdit = new QLineEdit();
    ui.customDirEdit->setObjectName("customDirEdit");
    ui.customDirEdit->setEnabled(false);
    ui.customDirEdit->setToolTip("Folder where all generated PGNs and analysis videos will be saved.");
    customDirHLayout->addWidget(ui.customDirEdit);

    auto* customDirBtn = new QPushButton("Browse...");
    customDirBtn->setEnabled(false);
    customDirBtn->setToolTip("Choose the folder where generated files should be saved.");
    customDirHLayout->addWidget(customDirBtn);

    outputDirLayout->addLayout(customDirHLayout);
    generalLayout->addWidget(outputDirGroup);

    connect(ui.sameAsSourceRadio, &QRadioButton::toggled, [this, customDirBtn](bool checked) {
        ui.customDirEdit->setEnabled(!checked);
        customDirBtn->setEnabled(!checked);
    });
    connect(ui.sameAsSourceRadio, &QRadioButton::toggled, this, [this]() { saveSettings(); });
    connect(ui.customDirEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    connect(customDirBtn, &QPushButton::clicked, this, [this]() {
        QSettings qs;
        QString lastDir = qs.value("lastCustomDir", QDir::homePath()).toString();
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", lastDir);
        if (!dir.isEmpty()) {
            ui.customDirEdit->setText(dir);
            qs.setValue("lastCustomDir", dir);
        }
    });

    // Generation Options
    auto* togglesGroup = new QGroupBox("Files to Create");
    togglesGroup->setToolTip("Choose which files ChessTube Analyzer should create for each video.");
    auto* togglesLayout = new QVBoxLayout(togglesGroup);
    togglesLayout->addWidget(createHelpText(
        "Start with the moves-only PGN. Move labels and engine-backed video overlays will run Stockfish automatically.",
        "Explains the recommended output choices."
    ));
    togglesLayout->addWidget(createToggleRow("Moves-only PGN", "Create a compact PGN containing the extracted legal moves.", ui.pgnExportToggle, true));
    togglesLayout->addWidget(createToggleRow("Move subtitles", "Embed synced move subtitles into the analysis video.", ui.subtitlesToggle, false));

    auto* annotationsRow = createToggleRow("Move quality labels", "Add familiar labels such as Book, !!, !, ?!, and ? to the PGN and video text. This runs Stockfish automatically.", ui.moveAnnotationsToggle, true);
    ui.moveAnnotationsToggle->setObjectName("moveAnnotationsToggle");
    togglesLayout->addWidget(annotationsRow);

    togglesLayout->addWidget(createToggleRow("Analysis video", "Create a new video with board, evaluation, and engine overlays.", ui.analysisVideoToggle, false));
    generalLayout->addWidget(togglesGroup);

    auto* cleanupGroup = new QGroupBox("Cleanup");
    cleanupGroup->setToolTip("Options for cleaning up files after processing.");
    auto* cleanupLayout = new QVBoxLayout(cleanupGroup);
    auto* removeRow = createToggleRow("Delete original video", "Delete the source video file after successful processing.", ui.removeOriginalToggle, false);
    ui.removeOriginalToggle->setObjectName("removeOriginalToggle");
    cleanupLayout->addWidget(removeRow);
    generalLayout->addWidget(cleanupGroup);

    // Theme selector
    auto* themeGroup = new QGroupBox("Appearance");
    themeGroup->setToolTip("Customize the look and feel of the application");
    auto* themeLayout = new QVBoxLayout(themeGroup);
    ui.themeComboBox = createComboBoxRow(themeLayout, "Theme:", "Choose the application's visual color theme", "", "Choose the application's visual color theme");
    ui.themeComboBox->addItems({"System", "Light", "Dark"});
    generalLayout->addWidget(themeGroup);

    generalLayout->addStretch();
    tabWidget->addTab(generalTab, "General");

    // === Tab 2: Video Export ===
    auto* videoExportTab = new QWidget();
    auto* videoExportLayout = new QVBoxLayout(videoExportTab);
    auto* encodingGroup = new QGroupBox("Analysis Video Export");
    encodingGroup->setToolTip("Configure the video file created when Analysis Video is enabled.");
    auto* encodingLayout = new QVBoxLayout(encodingGroup);
    encodingLayout->addWidget(createHelpText(
        "Recommended: H.264, original audio, .mp4, source resolution, and Standard quality. Change these mainly for smaller files, GPU encoding, or a specific upload target.",
        "Beginner guidance for choosing video export settings."
    ));

    ui.videoCodecComboBox = createComboBoxRow(encodingLayout, "Video encoding:", "Controls how the analysis video is compressed. H.264 is the safest choice for playback and uploads.", "videoCodecComboBox", "Choose the video encoder. Use H.264 CPU if you are unsure; use NVIDIA options only on systems with supported NVIDIA hardware.");
    ui.videoCodecComboBox->addItem("H.264 CPU (recommended compatibility)", "libx264");
    ui.videoCodecComboBox->addItem("H.264 NVIDIA GPU (fast if supported)", "h264_nvenc");
    ui.videoCodecComboBox->addItem("HEVC CPU (smaller files, less compatible)", "libx265");
    ui.videoCodecComboBox->addItem("HEVC NVIDIA GPU (fast, less compatible)", "hevc_nvenc");
    ui.videoCodecComboBox->addItem("VP9 (web-focused, slower)", "libvpx-vp9");

    ui.audioCodecComboBox = createComboBoxRow(encodingLayout, "Audio:", "Choose whether to keep the original audio or convert it.", "audioCodecComboBox", "Keeping original audio is fastest and avoids quality loss. Use AAC for broad MP4 compatibility.");
    ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
    ui.audioCodecComboBox->addItem("AAC audio (standard)", "aac");

    ui.extensionComboBox = createComboBoxRow(encodingLayout, "File type:", "Choose the file extension for the exported analysis video.", "extensionComboBox", "Choose the output video container. MP4 is recommended for most users.");
    ui.extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});

    ui.resolutionComboBox = createComboBoxRow(encodingLayout, "Size:", "Choose the exported video size. Keeping the source size preserves the original detail.", "resolutionComboBox", "Scaling down can save time and disk space. Same as source keeps the video closest to the original.");
    ui.resolutionComboBox->addItem("Same as source (recommended)", "Source Resolution");
    ui.resolutionComboBox->addItem("4K (3840x2160)", "4K");
    ui.resolutionComboBox->addItem("1080p (1920x1080)", "1080p");
    ui.resolutionComboBox->addItem("720p (1280x720)", "720p");

    ui.qualityComboBox = createComboBoxRow(encodingLayout, "Quality vs file size:", "Controls video compression. Higher quality creates larger files; smaller files look softer.", "qualityComboBox", "This maps to FFmpeg CRF values. Best quality uses larger files; Standard is the recommended balance; smaller-file choices can introduce blur or blockiness.");
    ui.qualityComboBox->addItem("Best quality, largest file", 18);
    ui.qualityComboBox->addItem("High quality", 20);
    ui.qualityComboBox->addItem("Standard (recommended)", 23);
    ui.qualityComboBox->addItem("Smaller file", 28);
    ui.qualityComboBox->addItem("Smallest file, visibly softer", 35);

    videoExportLayout->addWidget(encodingGroup);
    videoExportLayout->addStretch();
    tabWidget->addTab(videoExportTab, "Video Export");

    // === Tab 3: Stockfish ===
    auto* stockfishTab = new QWidget();
    auto* stockfishLayout = new QVBoxLayout(stockfishTab);
    ui.stockfishSettingsGroup = new QGroupBox("Engine Analysis");
    ui.stockfishSettingsGroup->setToolTip("Choose how much Stockfish analysis to add to PGNs and analysis videos.");
    auto* stockfishOptionsLayout = new QVBoxLayout(ui.stockfishSettingsGroup);

    stockfishOptionsLayout->addWidget(createHelpText(
        "Recommended: 1-2 lines, Normal strength, no time limit, no search limit. Increase strength or lines when accuracy matters more than waiting time.",
        "Beginner guidance for balancing Stockfish strength against processing time"
    ));

    auto* fpRow = createToggleRow("Quick engine analysis", "Use fast, lighter Stockfish settings. Good for a first pass; manual strength and limit controls below are temporarily ignored.", ui.fastPreviewToggle, false);
    ui.fastPreviewToggle->setObjectName("fastPreviewToggle");
    stockfishOptionsLayout->addWidget(fpRow);

    connect(ui.fastPreviewToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        ui.depthComboBox->setEnabled(!checked);
        ui.timeComboBox->setEnabled(!checked);
        ui.nodesComboBox->setEnabled(!checked);
        emit logMessage(checked ? "Quick engine analysis enabled (manual limits bypassed)" : "Quick engine analysis disabled (using manual limits)");
        saveSettings();
    });

    auto* stockfishPathLayout = new QHBoxLayout();
    stockfishPathLayout->addWidget(createSettingsLabel(
        "Stockfish location:",
        "The Stockfish executable ChessTube Analyzer will run for engine analysis."
    ));
    ui.stockfishPathEdit = new QLineEdit();
    ui.stockfishPathEdit->setObjectName("stockfishPathEdit");
    ui.stockfishPathEdit->setToolTip("Path to the Stockfish executable. Leave blank only if Stockfish is bundled with the app or already found automatically.");
    stockfishPathLayout->addWidget(ui.stockfishPathEdit);
    ui.stockfishPathBtn = new QPushButton("Browse...");
    ui.stockfishPathBtn->setToolTip("Choose the Stockfish executable manually.");
    stockfishPathLayout->addWidget(ui.stockfishPathBtn);
    ui.stockfishSearchBtn = new QPushButton("Auto-Find");
    ui.stockfishSearchBtn->setToolTip("Search common install and download folders for Stockfish.");
    stockfishPathLayout->addWidget(ui.stockfishSearchBtn);
    stockfishOptionsLayout->addLayout(stockfishPathLayout);

    ui.multiPvComboBox = createComboBoxRow(stockfishOptionsLayout, "Engine lines:", "How many suggested moves Stockfish should show for each position.", "", "More lines show alternatives, but each position takes longer to analyze.");
    ui.multiPvComboBox->addItem("1 line (fastest, clearest)", 1);
    ui.multiPvComboBox->addItem("2 lines (recommended comparison)", 2);
    ui.multiPvComboBox->addItem("3 lines (more alternatives)", 3);
    ui.multiPvComboBox->addItem("4 lines (slowest)", 4);

    auto* depthLabel = createSettingsLabel("Normal is the best starting point.", "A simple rule of thumb for choosing Stockfish strength.");
    ui.depthComboBox = createComboBoxRow(stockfishOptionsLayout, "Analysis strength:", "How deeply Stockfish searches. Stronger analysis takes longer.", "depthComboBox", "Normal is a good default. Strong and above can improve accuracy, but may add a lot of processing time.", depthLabel);
    ui.depthComboBox->addItem("Fast (Depth 10)", 10);
    ui.depthComboBox->addItem("Normal (Depth 15, recommended)", 15);
    ui.depthComboBox->addItem("Strong (Depth 20)", 20);
    ui.depthComboBox->addItem("Very strong (Depth 25)", 25);
    ui.depthComboBox->addItem("Deep (Depth 30)", 30);
    
    ui.timeComboBox = createComboBoxRow(stockfishOptionsLayout, "Time cap:", "Optional cap on how long Stockfish may spend on each board position.", "timeComboBox", "Optional safety cap per position. No cap lets Stockfish stop when it reaches the selected strength.");
    ui.timeComboBox->addItem("No cap (use strength setting)", 0);
    ui.timeComboBox->addItem("1 second", 1);
    ui.timeComboBox->addItem("2 seconds", 2);
    ui.timeComboBox->addItem("5 seconds", 5);
    ui.timeComboBox->addItem("10 seconds", 10);
    ui.timeComboBox->addItem("30 seconds", 30);
    ui.timeComboBox->addItem("60 seconds", 60);

    ui.nodesComboBox = createComboBoxRow(stockfishOptionsLayout, "Search work limit:", "Advanced cap on how many positions Stockfish may examine.", "nodesComboBox", "Advanced Stockfish limit. Leave this at No limit unless you are intentionally bounding CPU work.");
    ui.nodesComboBox->addItem("No limit (recommended)", 0);
    ui.nodesComboBox->addItem("100,000 nodes", 100000);
    ui.nodesComboBox->addItem("500,000 nodes", 500000);
    ui.nodesComboBox->addItem("1,000,000 nodes", 1000000);
    ui.nodesComboBox->addItem("5,000,000 nodes", 5000000);

    ui.analysisDepthComboBox = createComboBoxRow(stockfishOptionsLayout, "Line length:", "How many moves of each suggested Stockfish continuation should appear in PGN and video text.", "analysisDepthComboBox", "Shorter lines are easier to read. Longer lines provide more engine detail.");
    ui.analysisDepthComboBox->addItem("1 move", 1);
    ui.analysisDepthComboBox->addItem("2 moves", 2);
    ui.analysisDepthComboBox->addItem("3 moves", 3);
    ui.analysisDepthComboBox->addItem("4 moves", 4);
    ui.analysisDepthComboBox->addItem("5 moves", 5);
    ui.analysisDepthComboBox->addItem("8 moves", 8);
    ui.analysisDepthComboBox->addItem("10 moves", 10);

    stockfishLayout->addWidget(ui.stockfishSettingsGroup);
    stockfishLayout->addStretch();
    tabWidget->addTab(stockfishTab, "Stockfish");

    // === Tab 4: Advanced ===
    auto* advancedTab = new QWidget();
    auto* advancedLayout = new QVBoxLayout(advancedTab);
    auto* advancedGroup = new QGroupBox("Performance and Troubleshooting");
    advancedGroup->setToolTip("Tune processing speed, memory use, and debug output.");
    auto* advancedGroupLayout = new QVBoxLayout(advancedGroup);
    advancedGroupLayout->addWidget(createHelpText(
        "Most users can leave these on their defaults. Lower threads or RAM budget if your computer feels overloaded while processing.",
        "Explains when to change advanced settings."
    ));

    ui.threadComboBox = createComboBoxRow(advancedGroupLayout, "Video decoding threads:", "CPU threads used while reading video frames. More can be faster but may make the computer feel busier.", "", "Set the number of CPU threads used to decode video. Maximum uses all detected logical CPU threads.");
    ui.threadComboBox->clear(); // Need to clear the default items added by createComboBoxRow (if any) and re-populate
    const int maxThreads = cta::SysUtils::max_hardware_thread_count();
    for (int threadCount = 1; threadCount <= maxThreads; ++threadCount) {
        const QString label = threadCount == maxThreads
            ? QString("%1 threads (maximum, default)").arg(threadCount)
            : QString("%1 thread%2").arg(threadCount).arg(threadCount == 1 ? "" : "s");
        ui.threadComboBox->addItem(label, threadCount);
    }

    ui.debugLevelComboBox = createComboBoxRow(advancedGroupLayout, "Debug images:", "Save diagnostic images while processing. Useful when checking why a move was or was not detected.", "", "Choose how many debug images to save. Full diagnostics can use significant disk space.");
    ui.debugLevelComboBox->addItems({"None (recommended)", "Moves only", "Full diagnostics"});

    ui.memoryLimitComboBox = createComboBoxRow(advancedGroupLayout, "RAM budget:", "Limit parallel video workers to control peak memory use.", "memoryLimitComboBox", "Caps parallel workers to reduce peak RAM use. No limit is fastest, but can use more memory on long or high-resolution videos.");
    ui.memoryLimitComboBox->addItem("No limit (fastest)", 0);
    ui.memoryLimitComboBox->addItem("1 GB", 1024);
    ui.memoryLimitComboBox->addItem("2 GB", 2048);
    ui.memoryLimitComboBox->addItem("4 GB", 4096);
    ui.memoryLimitComboBox->addItem("8 GB", 8192);
    ui.memoryLimitComboBox->addItem("16 GB", 16384);

    auto* loggingRow1 = new QHBoxLayout();
    loggingRow1->addWidget(createSettingsLabel("Keep previous logs:", "How many old log files to keep in the AppData folder."));
    ui.logRetentionSpinBox = new QSpinBox();
    ui.logRetentionSpinBox->setObjectName("logRetentionSpinBox");
    ui.logRetentionSpinBox->setRange(1, 100);
    ui.logRetentionSpinBox->setToolTip("Number of log files to retain during log cycling.");
    connect(ui.logRetentionSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    loggingRow1->addWidget(ui.logRetentionSpinBox);

    ui.compressLogsCheck = new QCheckBox("Compress rotated logs (.zip)");
    ui.compressLogsCheck->setObjectName("compressLogsCheck");
    ui.compressLogsCheck->setToolTip("Automatically zip old log files to save disk space.");
    connect(ui.compressLogsCheck, &QCheckBox::toggled, this, [this]() { saveSettings(); });
    loggingRow1->addWidget(ui.compressLogsCheck);
    loggingRow1->addStretch();
    advancedGroupLayout->addLayout(loggingRow1);
    
    auto* loggingRow2 = new QHBoxLayout();
    auto* openLogsBtn = new QPushButton("Open Logs");
    openLogsBtn->setToolTip("Open the folder containing application log files.");
    connect(openLogsBtn, &QPushButton::clicked, this, []() {
        QString logDirStr = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath("logs");
        QDir().mkpath(logDirStr);
        QDesktopServices::openUrl(QUrl::fromLocalFile(logDirStr));
    });
    loggingRow2->addWidget(openLogsBtn);

    auto* logsStatusLabel = new QLabel("");
    logsStatusLabel->setObjectName("logsStatusLabel");

    auto* clearLogsBtn = new QPushButton("Clear Logs");
    clearLogsBtn->setToolTip("Delete all current log files to free up space.");
    connect(clearLogsBtn, &QPushButton::clicked, this, [this, logsStatusLabel]() {
        QString logDirStr = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath("logs");
        QDir logDir(logDirStr);
        QFileInfoList fileList = logDir.entryInfoList(QStringList() << "log_*.txt" << "log_*.zip", QDir::Files | QDir::NoSymLinks);
        int deleted = 0;
        for (const QFileInfo& fileInfo : fileList) {
            if (QFile::remove(fileInfo.absoluteFilePath())) {
                deleted++;
            }
        }
        logsStatusLabel->setText(QString("✓ Cleared %1 file(s)").arg(deleted));
        QTimer::singleShot(4000, logsStatusLabel, [logsStatusLabel]() { logsStatusLabel->setText(""); });
        emit logMessage(QString("Cleared %1 log file(s).").arg(deleted));
    });
    loggingRow2->addWidget(clearLogsBtn);
    loggingRow2->addWidget(logsStatusLabel);

    loggingRow2->addStretch();
    advancedGroupLayout->addLayout(loggingRow2);

    advancedLayout->addWidget(advancedGroup);
    advancedLayout->addStretch();
    tabWidget->addTab(advancedTab, "Advanced");

    auto* dialogBtnBox = new QDialogButtonBox(QDialogButtonBox::Close);
    dialogBtnBox->setToolTip("Close the settings window. Changes are saved automatically.");
    connect(dialogBtnBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    dialogLayout->addWidget(dialogBtnBox);

    // Connect signals
    connect(ui.pgnExportToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "PGN export enabled" : "PGN export disabled");
        saveSettings();
    });
    connect(ui.subtitlesToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Move subtitles enabled" : "Move subtitles disabled");
        if (checked && !ui.analysisVideoToggle->isChecked()) {
            ui.analysisVideoToggle->setChecked(true);
        }
        saveSettings();
    });
    connect(ui.analysisVideoToggle, &ToggleSwitch::toggled, this, [this](bool checked) { 
        emit logMessage(checked ? "Analysis Video generation enabled" : "Analysis Video generation disabled");
        if (!checked && ui.subtitlesToggle->isChecked()) {
            ui.subtitlesToggle->setChecked(false);
        }
        saveSettings(); 
    });
    connect(ui.removeOriginalToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Original video will be deleted after processing" : "Original video will be kept after processing");
        saveSettings();
    });
    connect(ui.threadComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
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
    
    connect(ui.moveAnnotationsToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Move quality annotations enabled" : "Move quality annotations disabled");
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

void SettingsDialog::loadSettings() {
    QSettings settings;
    const auto widgets = this->findChildren<QWidget*>();
    for (auto* w : widgets) w->blockSignals(true);

    ui.pgnExportToggle->setChecked(settings.value("generatePgn", true).toBool());
    ui.subtitlesToggle->setChecked(settings.value("generateSubtitles", false).toBool());
    ui.analysisVideoToggle->setChecked(settings.value("generateAnalysisVideo", false).toBool());
    ui.moveAnnotationsToggle->setChecked(settings.value("analysis/enableMoveAnnotations", true).toBool());

    int multiPv = settings.value("multiPv", 3).toInt();
    int multiPvIdx = ui.multiPvComboBox->findData(multiPv);
    ui.multiPvComboBox->setCurrentIndex(multiPvIdx >= 0 ? multiPvIdx : 2);
    const int defaultThreads = cta::SysUtils::max_hardware_thread_count();
    setThreadComboValue(ui.threadComboBox, settings.value("ffmpegThreads", defaultThreads).toInt());
    
    { int idx = ui.depthComboBox->findData(settings.value("stockfishDepth", 15).toInt()); ui.depthComboBox->setCurrentIndex(idx >= 0 ? idx : ui.depthComboBox->findData(15)); }
    { int idx = ui.timeComboBox->findData(settings.value("stockfishTime", 0).toInt()); ui.timeComboBox->setCurrentIndex(idx >= 0 ? idx : ui.timeComboBox->findData(0)); }
    { int idx = ui.nodesComboBox->findData(settings.value("stockfishNodes", 0).toInt()); ui.nodesComboBox->setCurrentIndex(idx >= 0 ? idx : ui.nodesComboBox->findData(0)); }
    { int idx = ui.analysisDepthComboBox->findData(settings.value("stockfishAnalysisDepth", 5).toInt()); ui.analysisDepthComboBox->setCurrentIndex(idx >= 0 ? idx : ui.analysisDepthComboBox->findData(5)); }
    ui.stockfishPathEdit->setText(settings.value("stockfishPath", "").toString());
    
    bool fpEnabled = settings.value("fastPreview", false).toBool();
    ui.fastPreviewToggle->setChecked(fpEnabled);
    ui.depthComboBox->setEnabled(!fpEnabled);
    ui.timeComboBox->setEnabled(!fpEnabled);
    ui.nodesComboBox->setEnabled(!fpEnabled);

    ui.debugLevelComboBox->setCurrentIndex(settings.value("debugLevel", 0).toInt());
    ui.themeComboBox->setCurrentIndex(settings.value("themeMode", 0).toInt());

    bool sameAsSource = settings.value("outSameAsSource", true).toBool();
    ui.sameAsSourceRadio->setChecked(sameAsSource);
    ui.customDirRadio->setChecked(!sameAsSource);
    ui.customDirEdit->setText(settings.value("outCustomDir", "").toString());
    ui.defaultVideoDirEdit->setText(settings.value("defaultVideoDir", "").toString());

    const QString videoCodec = canonicalVideoCodec(settings.value("videoCodec", "libx264").toString());
    int vIdx = ui.videoCodecComboBox->findData(videoCodec);
    ui.videoCodecComboBox->setCurrentIndex(vIdx >= 0 ? vIdx : 0);
    
    // Ensure dependent comboboxes have the correct items for the loaded codec before setting their values
    ui.audioCodecComboBox->clear(); ui.extensionComboBox->clear();
    if (ui.videoCodecComboBox->currentData().toString() == "libvpx-vp9") {
        ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
        ui.audioCodecComboBox->addItem("Opus audio (recommended for WebM)", "libopus");
        ui.extensionComboBox->addItems({".webm", ".mkv"});
    } else {
        ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
        ui.audioCodecComboBox->addItem("AAC audio (standard)", "aac");
        ui.extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
    }
    
    const QString audioCodec = canonicalAudioCodec(settings.value("audioCodec", "copy").toString());
    int aIdx = ui.audioCodecComboBox->findData(audioCodec);
    ui.audioCodecComboBox->setCurrentIndex(aIdx >= 0 ? aIdx : 0);
    
    int eIdx = ui.extensionComboBox->findText(settings.value("videoExtension", ".mp4").toString());
    ui.extensionComboBox->setCurrentIndex(eIdx >= 0 ? eIdx : 0);
    
    const QString resolution = canonicalResolution(settings.value("videoResolution", "Source Resolution").toString());
    int rIdx = ui.resolutionComboBox->findData(resolution);
    ui.resolutionComboBox->setCurrentIndex(rIdx >= 0 ? rIdx : 0);
    
    int qval = settings.value("videoQuality", 23).toInt();
    int qidx = ui.qualityComboBox->findData(qval);
    ui.qualityComboBox->setCurrentIndex(qidx >= 0 ? qidx : 2);

    { int idx = ui.memoryLimitComboBox->findData(settings.value("memoryLimitMB", 0).toInt()); ui.memoryLimitComboBox->setCurrentIndex(idx >= 0 ? idx : ui.memoryLimitComboBox->findData(0)); }

    ui.logRetentionSpinBox->setValue(settings.value("logRetentionCount", 10).toInt());
    ui.compressLogsCheck->setChecked(settings.value("compressOldLogs", false).toBool());
    ui.removeOriginalToggle->setChecked(settings.value("removeOriginalVideo", false).toBool());

    for (auto* w : widgets) w->blockSignals(false);
}

void SettingsDialog::saveSettings() {
    QSettings settings;
    settings.setValue("generatePgn", ui.pgnExportToggle->isChecked());
    settings.setValue("generateSubtitles", ui.subtitlesToggle->isChecked());
    settings.setValue("generateAnalysisVideo", ui.analysisVideoToggle->isChecked());
    settings.setValue("multiPv", ui.multiPvComboBox->currentData().toInt());
    settings.setValue("ffmpegThreads", ui.threadComboBox->currentData().toInt());
    settings.setValue("themeMode", ui.themeComboBox->currentIndex());
    settings.setValue("analysis/enableMoveAnnotations", ui.moveAnnotationsToggle->isChecked());
    settings.setValue("removeOriginalVideo", ui.removeOriginalToggle->isChecked());

    settings.setValue("stockfishDepth", ui.depthComboBox->currentData().toInt());
    settings.setValue("stockfishTime", ui.timeComboBox->currentData().toInt());
    settings.setValue("stockfishNodes", ui.nodesComboBox->currentData().toInt());
    settings.setValue("stockfishAnalysisDepth", ui.analysisDepthComboBox->currentData().toInt());
    settings.setValue("stockfishPath", ui.stockfishPathEdit->text());
    settings.setValue("debugLevel", ui.debugLevelComboBox->currentIndex());

    settings.setValue("fastPreview", ui.fastPreviewToggle->isChecked());

    settings.setValue("outSameAsSource", ui.sameAsSourceRadio->isChecked());
    settings.setValue("outCustomDir", ui.customDirEdit->text());
    settings.setValue("defaultVideoDir", ui.defaultVideoDirEdit->text());
    settings.setValue("videoCodec", ui.videoCodecComboBox->currentData().toString());
    settings.setValue("audioCodec", ui.audioCodecComboBox->currentData().toString());
    settings.setValue("videoExtension", ui.extensionComboBox->currentText());
    settings.setValue("videoResolution", ui.resolutionComboBox->currentData().toString());
    settings.setValue("videoQuality", ui.qualityComboBox->currentData().toInt());
    
    settings.setValue("memoryLimitMB", ui.memoryLimitComboBox->currentData().toInt());
    settings.setValue("logRetentionCount", ui.logRetentionSpinBox->value());
    settings.setValue("compressOldLogs", ui.compressLogsCheck->isChecked());
}

void SettingsDialog::populateSettings(ProcessingSettings& s) const {
    const bool enableMoveAnnotations = ui.moveAnnotationsToggle->isChecked();

    s.generatePgn = ui.pgnExportToggle->isChecked();
    s.generateSubtitles = ui.subtitlesToggle->isChecked();
    s.enableMoveAnnotations = enableMoveAnnotations;
    s.enableStockfish = enableMoveAnnotations;
    s.generateAnalysisVideo = ui.analysisVideoToggle->isChecked() || ui.subtitlesToggle->isChecked();
    s.multiPv = ui.multiPvComboBox->currentData().toInt();
    s.ffmpegThreads = ui.threadComboBox->currentData().toInt();
    s.stockfishDepth = ui.depthComboBox->currentData().toInt();
    s.stockfishTime = ui.timeComboBox->currentData().toInt();
    s.stockfishNodes = ui.nodesComboBox->currentData().toInt();
    s.stockfishAnalysisDepth = ui.analysisDepthComboBox->currentData().toInt();
    s.stockfishPath = ui.stockfishPathEdit->text();
    s.debugLevel = ui.debugLevelComboBox->currentIndex();
    s.memoryLimitMB = ui.memoryLimitComboBox->currentData().toInt();

    if (ui.fastPreviewToggle->isChecked()) {
        s.stockfishDepth = 10;
        s.stockfishTime = 2; // 2 seconds max
        s.stockfishNodes = 0;
    }
}

void SettingsDialog::applySettingsToUi(const ProcessingSettings& settings) {
    ui.pgnExportToggle->setChecked(settings.generatePgn);
    ui.subtitlesToggle->setChecked(settings.generateSubtitles);
    ui.moveAnnotationsToggle->setChecked(settings.enableMoveAnnotations);
    ui.analysisVideoToggle->setChecked(settings.generateAnalysisVideo);
    int idx = ui.multiPvComboBox->findData(settings.multiPv);
    if (idx >= 0) ui.multiPvComboBox->setCurrentIndex(idx);
    setThreadComboValue(ui.threadComboBox, settings.ffmpegThreads);
    { int idx = ui.depthComboBox->findData(settings.stockfishDepth); if (idx >= 0) ui.depthComboBox->setCurrentIndex(idx); }
    { int idx = ui.timeComboBox->findData(settings.stockfishTime); if (idx >= 0) ui.timeComboBox->setCurrentIndex(idx); }
    { int idx = ui.nodesComboBox->findData(settings.stockfishNodes); if (idx >= 0) ui.nodesComboBox->setCurrentIndex(idx); }
    { int idx = ui.analysisDepthComboBox->findData(settings.stockfishAnalysisDepth); if (idx >= 0) ui.analysisDepthComboBox->setCurrentIndex(idx); }
    ui.stockfishPathEdit->setText(settings.stockfishPath);
    ui.debugLevelComboBox->setCurrentIndex(settings.debugLevel);
}

void SettingsDialog::applyHeadlessOverrides(int pgnOverride, int analysisVideoOverride, int moveLabelsOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, int memoryLimit) {
    if (pgnOverride != -1) ui.pgnExportToggle->setChecked(pgnOverride != 0);
    if (analysisVideoOverride != -1) ui.analysisVideoToggle->setChecked(analysisVideoOverride != 0);
    if (moveLabelsOverride != -1) ui.moveAnnotationsToggle->setChecked(moveLabelsOverride != 0);
    if (multiPv > 0) {
        int idx = ui.multiPvComboBox->findData(multiPv);
        if (idx >= 0) ui.multiPvComboBox->setCurrentIndex(idx);
    }
    if (threads > 0) setThreadComboValue(ui.threadComboBox, threads);
    if (depth > 0) { int idx = ui.depthComboBox->findData(depth); if (idx >= 0) ui.depthComboBox->setCurrentIndex(idx); }
    if (time >= 0) { int idx = ui.timeComboBox->findData(time); if (idx >= 0) ui.timeComboBox->setCurrentIndex(idx); }
    if (nodes >= 0) { int idx = ui.nodesComboBox->findData(nodes); if (idx >= 0) ui.nodesComboBox->setCurrentIndex(idx); }
    if (analysisDepth > 0) { int idx = ui.analysisDepthComboBox->findData(analysisDepth); if (idx >= 0) ui.analysisDepthComboBox->setCurrentIndex(idx); }
    if (memoryLimit >= 0) { int idx = ui.memoryLimitComboBox->findData(memoryLimit); if (idx >= 0) ui.memoryLimitComboBox->setCurrentIndex(idx); }
    if (!debugLevelStr.isEmpty()) {
        if (debugLevelStr == "NONE") ui.debugLevelComboBox->setCurrentIndex(0);
        else if (debugLevelStr == "MOVES") ui.debugLevelComboBox->setCurrentIndex(1);
        else if (debugLevelStr == "FULL") ui.debugLevelComboBox->setCurrentIndex(2);
    }
}

} // namespace cta
