#include "SettingsDialog.h"
#include "ToggleSwitch.h"
#include "ThemeManager.h"

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

int maxHardwareThreadCount() {
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    return std::max(1, static_cast<int>(hardwareThreads));
}

void setThreadComboValue(QComboBox* comboBox, int threads) {
    if (!comboBox) {
        return;
    }

    const int clampedThreads = std::clamp(threads, 1, maxHardwareThreadCount());
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

    auto* outputDirGroup = new QGroupBox("Where to Save Results");
    outputDirGroup->setToolTip("Choose where PGN files and analysis videos will be saved.");
    auto* outputDirLayout = new QVBoxLayout(outputDirGroup);
    outputDirLayout->addWidget(createHelpText(
        "Choose a default folder for files created after each video finishes.",
        "Explains how ChessTube Analyzer chooses the output folder."
    ));

    auto* sameAsSourceRadio = new QRadioButton("Next to each source video");
    sameAsSourceRadio->setObjectName("sameAsSourceRadio");
    sameAsSourceRadio->setChecked(true);
    sameAsSourceRadio->setToolTip("Save each video's output files in the same folder as that input video.");
    outputDirLayout->addWidget(sameAsSourceRadio);

    auto* customDirHLayout = new QHBoxLayout();
    auto* customDirRadio = new QRadioButton("One folder for all results:");
    customDirRadio->setObjectName("customDirRadio");
    customDirRadio->setToolTip("Save every output file to the folder you choose here.");
    customDirHLayout->addWidget(customDirRadio);

    auto* customDirEdit = new QLineEdit();
    customDirEdit->setObjectName("customDirEdit");
    customDirEdit->setEnabled(false);
    customDirEdit->setToolTip("Folder where all generated PGNs and analysis videos will be saved.");
    customDirHLayout->addWidget(customDirEdit);

    auto* customDirBtn = new QPushButton("Browse...");
    customDirBtn->setObjectName("customDirBtn");
    customDirBtn->setEnabled(false);
    customDirBtn->setToolTip("Choose the folder where generated files should be saved.");
    customDirHLayout->addWidget(customDirBtn);

    outputDirLayout->addLayout(customDirHLayout);
    generalLayout->addWidget(outputDirGroup);

    connect(sameAsSourceRadio, &QRadioButton::toggled, [customDirEdit, customDirBtn](bool checked) {
        customDirEdit->setEnabled(!checked);
        customDirBtn->setEnabled(!checked);
    });
    connect(sameAsSourceRadio, &QRadioButton::toggled, this, [this]() { saveSettings(); });
    connect(customDirEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    connect(customDirBtn, &QPushButton::clicked, this, [this, customDirEdit]() {
        QSettings qs;
        QString lastDir = qs.value("lastCustomDir", QDir::homePath()).toString();
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", lastDir);
        if (!dir.isEmpty()) {
            customDirEdit->setText(dir);
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
    togglesLayout->addWidget(createToggleRow("Moves-only PGN", "Create a compact PGN containing the extracted legal moves.", pgnExportToggle_, true));
    togglesLayout->addWidget(createToggleRow("Move subtitles", "Embed synced move subtitles into the analysis video.", subtitlesToggle_, false));

    ToggleSwitch* moveAnnotationsToggle = nullptr;
    auto* annotationsRow = createToggleRow("Move quality labels", "Add familiar labels such as Book, !!, !, ?!, and ? to the PGN and video text. This runs Stockfish automatically.", moveAnnotationsToggle, true);
    moveAnnotationsToggle->setObjectName("moveAnnotationsToggle");
    togglesLayout->addWidget(annotationsRow);

    togglesLayout->addWidget(createToggleRow("Analysis video", "Create a new video with board, evaluation, and engine overlays.", analysisVideoToggle_, false));
    generalLayout->addWidget(togglesGroup);

    auto* cleanupGroup = new QGroupBox("Cleanup");
    cleanupGroup->setToolTip("Options for cleaning up files after processing.");
    auto* cleanupLayout = new QVBoxLayout(cleanupGroup);
    ToggleSwitch* removeOriginalToggle = nullptr;
    auto* removeRow = createToggleRow("Delete original video", "Delete the source video file after successful processing.", removeOriginalToggle, false);
    removeOriginalToggle->setObjectName("removeOriginalToggle");
    cleanupLayout->addWidget(removeRow);
    generalLayout->addWidget(cleanupGroup);

    // Theme selector
    auto* themeGroup = new QGroupBox("Appearance");
    themeGroup->setToolTip("Customize the look and feel of the application");
    auto* themeLayout = new QHBoxLayout(themeGroup);
    auto* themeLabel = createSettingsLabel("Theme:", "Choose the application's visual color theme");
    themeLayout->addWidget(themeLabel);
    themeComboBox_ = new QComboBox();
    themeComboBox_->addItems({"System", "Light", "Dark"});
    themeComboBox_->setProperty("class", "dropdown");
    themeComboBox_->setToolTip("Choose the application's visual color theme");
    themeLayout->addWidget(themeComboBox_);
    themeLayout->addStretch();
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

    auto* videoCodecLayout = new QHBoxLayout();
    videoCodecLayout->addWidget(createSettingsLabel("Video encoding:", "Controls how the analysis video is compressed. H.264 is the safest choice for playback and uploads."));
    auto* videoCodecComboBox = new QComboBox();
    videoCodecComboBox->setObjectName("videoCodecComboBox");
    videoCodecComboBox->addItem("H.264 CPU (recommended compatibility)", "libx264");
    videoCodecComboBox->addItem("H.264 NVIDIA GPU (fast if supported)", "h264_nvenc");
    videoCodecComboBox->addItem("HEVC CPU (smaller files, less compatible)", "libx265");
    videoCodecComboBox->addItem("HEVC NVIDIA GPU (fast, less compatible)", "hevc_nvenc");
    videoCodecComboBox->addItem("VP9 (web-focused, slower)", "libvpx-vp9");
    videoCodecComboBox->setProperty("class", "dropdown");
    videoCodecComboBox->setToolTip("Choose the video encoder. Use H.264 CPU if you are unsure; use NVIDIA options only on systems with supported NVIDIA hardware.");
    videoCodecLayout->addWidget(videoCodecComboBox);
    videoCodecLayout->addStretch();
    encodingLayout->addLayout(videoCodecLayout);

    auto* audioCodecLayout = new QHBoxLayout();
    audioCodecLayout->addWidget(createSettingsLabel("Audio:", "Choose whether to keep the original audio or convert it."));
    auto* audioCodecComboBox = new QComboBox();
    audioCodecComboBox->setObjectName("audioCodecComboBox");
    audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
    audioCodecComboBox->addItem("AAC audio (standard)", "aac");
    audioCodecComboBox->setProperty("class", "dropdown");
    audioCodecComboBox->setToolTip("Keeping original audio is fastest and avoids quality loss. Use AAC for broad MP4 compatibility.");
    audioCodecLayout->addWidget(audioCodecComboBox);
    audioCodecLayout->addStretch();
    encodingLayout->addLayout(audioCodecLayout);

    auto* extensionLayout = new QHBoxLayout();
    extensionLayout->addWidget(createSettingsLabel("File type:", "Choose the file extension for the exported analysis video."));
    auto* extensionComboBox = new QComboBox();
    extensionComboBox->setObjectName("extensionComboBox");
    extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
    extensionComboBox->setProperty("class", "dropdown");
    extensionComboBox->setToolTip("Choose the output video container. MP4 is recommended for most users.");
    extensionLayout->addWidget(extensionComboBox);
    extensionLayout->addStretch();
    encodingLayout->addLayout(extensionLayout);

    auto* resolutionLayout = new QHBoxLayout();
    resolutionLayout->addWidget(createSettingsLabel("Size:", "Choose the exported video size. Keeping the source size preserves the original detail."));
    auto* resolutionComboBox = new QComboBox();
    resolutionComboBox->setObjectName("resolutionComboBox");
    resolutionComboBox->addItem("Same as source (recommended)", "Source Resolution");
    resolutionComboBox->addItem("4K (3840x2160)", "4K");
    resolutionComboBox->addItem("1080p (1920x1080)", "1080p");
    resolutionComboBox->addItem("720p (1280x720)", "720p");
    resolutionComboBox->setProperty("class", "dropdown");
    resolutionComboBox->setToolTip("Scaling down can save time and disk space. Same as source keeps the video closest to the original.");
    resolutionLayout->addWidget(resolutionComboBox);
    resolutionLayout->addStretch();
    encodingLayout->addLayout(resolutionLayout);

    auto* qualityLayout = new QHBoxLayout();
    qualityLayout->addWidget(createSettingsLabel("Quality vs file size:", "Controls video compression. Higher quality creates larger files; smaller files look softer."));
    auto* qualityComboBox = new QComboBox();
    qualityComboBox->setObjectName("qualityComboBox");
    qualityComboBox->addItem("Best quality, largest file", 18);
    qualityComboBox->addItem("High quality", 20);
    qualityComboBox->addItem("Standard (recommended)", 23);
    qualityComboBox->addItem("Smaller file", 28);
    qualityComboBox->addItem("Smallest file, visibly softer", 35);
    qualityComboBox->setProperty("class", "dropdown");
    qualityComboBox->setToolTip("This maps to FFmpeg CRF values. Best quality uses larger files; Standard is the recommended balance; smaller-file choices can introduce blur or blockiness.");
    qualityLayout->addWidget(qualityComboBox);
    qualityLayout->addStretch();
    encodingLayout->addLayout(qualityLayout);

    videoExportLayout->addWidget(encodingGroup);
    videoExportLayout->addStretch();
    tabWidget->addTab(videoExportTab, "Video Export");

    // === Tab 3: Stockfish ===
    auto* stockfishTab = new QWidget();
    auto* stockfishLayout = new QVBoxLayout(stockfishTab);
    stockfishSettingsGroup_ = new QGroupBox("Engine Analysis");
    stockfishSettingsGroup_->setToolTip("Choose how much Stockfish analysis to add to PGNs and analysis videos.");
    auto* stockfishOptionsLayout = new QVBoxLayout(stockfishSettingsGroup_);

    stockfishOptionsLayout->addWidget(createHelpText(
        "Recommended: 1-2 lines, Normal strength, no time limit, no search limit. Increase strength or lines when accuracy matters more than waiting time.",
        "Beginner guidance for balancing Stockfish strength against processing time"
    ));

    ToggleSwitch* fastPreviewToggle = nullptr;
    auto* fpRow = createToggleRow("Quick engine analysis", "Use fast, lighter Stockfish settings. Good for a first pass; manual strength and limit controls below are temporarily ignored.", fastPreviewToggle, false);
    fastPreviewToggle->setObjectName("fastPreviewToggle");
    stockfishOptionsLayout->addWidget(fpRow);

    connect(fastPreviewToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
        if (auto* d = findChild<QComboBox*>("depthComboBox")) d->setEnabled(!checked);
        if (auto* t = findChild<QComboBox*>("timeComboBox")) t->setEnabled(!checked);
        if (auto* n = findChild<QComboBox*>("nodesComboBox")) n->setEnabled(!checked);
        emit logMessage(checked ? "Quick engine analysis enabled (manual limits bypassed)" : "Quick engine analysis disabled (using manual limits)");
        saveSettings();
    });

    auto* stockfishPathLayout = new QHBoxLayout();
    stockfishPathLayout->addWidget(createSettingsLabel(
        "Stockfish location:",
        "The Stockfish executable ChessTube Analyzer will run for engine analysis."
    ));
    auto* stockfishPathEdit = new QLineEdit();
    stockfishPathEdit->setObjectName("stockfishPathEdit");
    stockfishPathEdit->setToolTip("Path to the Stockfish executable. Leave blank only if Stockfish is bundled with the app or already found automatically.");
    stockfishPathLayout->addWidget(stockfishPathEdit);
    auto* stockfishPathBtn = new QPushButton("Browse...");
    stockfishPathBtn->setToolTip("Choose the Stockfish executable manually.");
    stockfishPathLayout->addWidget(stockfishPathBtn);
    auto* stockfishSearchBtn = new QPushButton("Auto-Find");
    stockfishSearchBtn->setToolTip("Search common install and download folders for Stockfish.");
    stockfishPathLayout->addWidget(stockfishSearchBtn);
    stockfishOptionsLayout->addLayout(stockfishPathLayout);

    auto* multiPvLayout = new QHBoxLayout();
    multiPvLayout->addWidget(createSettingsLabel(
        "Engine lines:",
        "How many suggested moves Stockfish should show for each position."
    ));
    multiPvComboBox_ = new QComboBox();
    multiPvComboBox_->addItem("1 line (fastest, clearest)", 1);
    multiPvComboBox_->addItem("2 lines (recommended comparison)", 2);
    multiPvComboBox_->addItem("3 lines (more alternatives)", 3);
    multiPvComboBox_->addItem("4 lines (slowest)", 4);
    multiPvComboBox_->setProperty("class", "dropdown");
    multiPvComboBox_->setToolTip("More lines show alternatives, but each position takes longer to analyze.");
    multiPvLayout->addWidget(multiPvComboBox_);
    multiPvLayout->addStretch();
    stockfishOptionsLayout->addLayout(multiPvLayout);

    auto* depthLayout = new QHBoxLayout();
    depthLayout->addWidget(createSettingsLabel(
        "Analysis strength:",
        "How deeply Stockfish searches. Stronger analysis takes longer."
    ));
    auto* depthComboBox = new QComboBox();
    depthComboBox->setObjectName("depthComboBox");
    depthComboBox->addItem("Fast (Depth 10)", 10);
    depthComboBox->addItem("Normal (Depth 15, recommended)", 15);
    depthComboBox->addItem("Strong (Depth 20)", 20);
    depthComboBox->addItem("Very strong (Depth 25)", 25);
    depthComboBox->addItem("Deep (Depth 30)", 30);
    depthComboBox->setProperty("class", "dropdown");
    depthComboBox->setToolTip("Normal is a good default. Strong and above can improve accuracy, but may add a lot of processing time.");
    depthLayout->addWidget(depthComboBox);
    depthLayout->addWidget(createSettingsLabel(
        "Normal is the best starting point.",
        "A simple rule of thumb for choosing Stockfish strength."
    ));
    depthLayout->addStretch();
    stockfishOptionsLayout->addLayout(depthLayout);
    
    auto* timeLayout = new QHBoxLayout();
    timeLayout->addWidget(createSettingsLabel(
        "Time cap:",
        "Optional cap on how long Stockfish may spend on each board position."
    ));
    auto* timeComboBox = new QComboBox();
    timeComboBox->setObjectName("timeComboBox");
    timeComboBox->addItem("No cap (use strength setting)", 0);
    timeComboBox->addItem("1 second", 1);
    timeComboBox->addItem("2 seconds", 2);
    timeComboBox->addItem("5 seconds", 5);
    timeComboBox->addItem("10 seconds", 10);
    timeComboBox->addItem("30 seconds", 30);
    timeComboBox->addItem("60 seconds", 60);
    timeComboBox->setProperty("class", "dropdown");
    timeComboBox->setToolTip("Optional safety cap per position. No cap lets Stockfish stop when it reaches the selected strength.");
    timeLayout->addWidget(timeComboBox);
    timeLayout->addStretch();
    stockfishOptionsLayout->addLayout(timeLayout);

    auto* nodesLayout = new QHBoxLayout();
    nodesLayout->addWidget(createSettingsLabel(
        "Search work limit:",
        "Advanced cap on how many positions Stockfish may examine."
    ));
    auto* nodesComboBox = new QComboBox();
    nodesComboBox->setObjectName("nodesComboBox");
    nodesComboBox->addItem("No limit (recommended)", 0);
    nodesComboBox->addItem("100,000 nodes", 100000);
    nodesComboBox->addItem("500,000 nodes", 500000);
    nodesComboBox->addItem("1,000,000 nodes", 1000000);
    nodesComboBox->addItem("5,000,000 nodes", 5000000);
    nodesComboBox->setProperty("class", "dropdown");
    nodesComboBox->setToolTip("Advanced Stockfish limit. Leave this at No limit unless you are intentionally bounding CPU work.");
    nodesLayout->addWidget(nodesComboBox);
    nodesLayout->addStretch();
    stockfishOptionsLayout->addLayout(nodesLayout);

    auto* analysisDepthLayout = new QHBoxLayout();
    analysisDepthLayout->addWidget(createSettingsLabel(
        "Line length:",
        "How many moves of each suggested Stockfish continuation should appear in PGN and video text."
    ));
    auto* analysisDepthComboBox = new QComboBox();
    analysisDepthComboBox->setObjectName("analysisDepthComboBox");
    analysisDepthComboBox->addItem("1 move", 1);
    analysisDepthComboBox->addItem("2 moves", 2);
    analysisDepthComboBox->addItem("3 moves", 3);
    analysisDepthComboBox->addItem("4 moves", 4);
    analysisDepthComboBox->addItem("5 moves", 5);
    analysisDepthComboBox->addItem("8 moves", 8);
    analysisDepthComboBox->addItem("10 moves", 10);
    analysisDepthComboBox->setProperty("class", "dropdown");
    analysisDepthComboBox->setToolTip("Shorter lines are easier to read. Longer lines provide more engine detail.");
    analysisDepthLayout->addWidget(analysisDepthComboBox);
    analysisDepthLayout->addStretch();
    stockfishOptionsLayout->addLayout(analysisDepthLayout);

    stockfishLayout->addWidget(stockfishSettingsGroup_);
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

    auto* threadLayout = new QHBoxLayout();
    threadLayout->addWidget(createSettingsLabel("Video decoding threads:", "CPU threads used while reading video frames. More can be faster but may make the computer feel busier."));
    threadComboBox_ = new QComboBox();
    const int maxThreads = maxHardwareThreadCount();
    for (int threadCount = 1; threadCount <= maxThreads; ++threadCount) {
        const QString label = threadCount == maxThreads
            ? QString("%1 threads (maximum, default)").arg(threadCount)
            : QString("%1 thread%2").arg(threadCount).arg(threadCount == 1 ? "" : "s");
        threadComboBox_->addItem(label, threadCount);
    }
    threadComboBox_->setProperty("class", "dropdown");
    threadComboBox_->setToolTip("Set the number of CPU threads used to decode video. Maximum uses all detected logical CPU threads.");
    threadLayout->addWidget(threadComboBox_);
    threadLayout->addStretch();
    advancedGroupLayout->addLayout(threadLayout);

    auto* debugLevelLayout = new QHBoxLayout();
    debugLevelLayout->addWidget(createSettingsLabel("Debug images:", "Save diagnostic images while processing. Useful when checking why a move was or was not detected."));
    debugLevelComboBox_ = new QComboBox();
    debugLevelComboBox_->addItems({"None (recommended)", "Moves only", "Full diagnostics"});
    debugLevelComboBox_->setProperty("class", "dropdown");
    debugLevelComboBox_->setToolTip("Choose how many debug images to save. Full diagnostics can use significant disk space.");
    debugLevelLayout->addWidget(debugLevelComboBox_);
    debugLevelLayout->addStretch();
    advancedGroupLayout->addLayout(debugLevelLayout);

    auto* memoryLimitLayout = new QHBoxLayout();
    memoryLimitLayout->addWidget(createSettingsLabel("RAM budget:", "Limit parallel video workers to control peak memory use."));
    auto* memoryLimitComboBox = new QComboBox();
    memoryLimitComboBox->setObjectName("memoryLimitComboBox");
    memoryLimitComboBox->addItem("No limit (fastest)", 0);
    memoryLimitComboBox->addItem("1 GB", 1024);
    memoryLimitComboBox->addItem("2 GB", 2048);
    memoryLimitComboBox->addItem("4 GB", 4096);
    memoryLimitComboBox->addItem("8 GB", 8192);
    memoryLimitComboBox->addItem("16 GB", 16384);
    memoryLimitComboBox->setProperty("class", "dropdown");
    memoryLimitComboBox->setToolTip("Caps parallel workers to reduce peak RAM use. No limit is fastest, but can use more memory on long or high-resolution videos.");
    memoryLimitLayout->addWidget(memoryLimitComboBox);
    memoryLimitLayout->addStretch();
    advancedGroupLayout->addLayout(memoryLimitLayout);

    auto* loggingRow1 = new QHBoxLayout();
    loggingRow1->addWidget(createSettingsLabel("Keep previous logs:", "How many old log files to keep in the AppData folder."));
    auto* logRetentionSpinBox = new QSpinBox();
    logRetentionSpinBox->setObjectName("logRetentionSpinBox");
    logRetentionSpinBox->setRange(1, 100);
    logRetentionSpinBox->setToolTip("Number of log files to retain during log cycling.");
    connect(logRetentionSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    loggingRow1->addWidget(logRetentionSpinBox);

    auto* compressLogsCheck = new QCheckBox("Compress rotated logs (.zip)");
    compressLogsCheck->setObjectName("compressLogsCheck");
    compressLogsCheck->setToolTip("Automatically zip old log files to save disk space.");
    connect(compressLogsCheck, &QCheckBox::toggled, this, [this]() { saveSettings(); });
    loggingRow1->addWidget(compressLogsCheck);
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
    connect(pgnExportToggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "PGN export enabled" : "PGN export disabled");
        saveSettings();
    });
    connect(subtitlesToggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "Move subtitles enabled" : "Move subtitles disabled");
        if (checked && !analysisVideoToggle_->isChecked()) {
            analysisVideoToggle_->setChecked(true);
        }
        saveSettings();
    });
    connect(analysisVideoToggle_, &ToggleSwitch::toggled, this, [this](bool checked) { 
        emit logMessage(checked ? "Analysis Video generation enabled" : "Analysis Video generation disabled");
        if (!checked && subtitlesToggle_->isChecked()) {
            subtitlesToggle_->setChecked(false);
        }
        saveSettings(); 
    });
    if (auto* rot = findChild<ToggleSwitch*>("removeOriginalToggle")) {
        connect(rot, &ToggleSwitch::toggled, this, [this](bool checked) {
            emit logMessage(checked ? "Original video will be deleted after processing" : "Original video will be kept after processing");
            saveSettings();
        });
    }
    connect(threadComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(multiPvComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    
    if (auto* stockfishPathBtn = findChild<QPushButton*>("stockfishPathBtn")) {
        connect(stockfishPathBtn, &QPushButton::clicked, this, [this]() {
            QSettings settings;
            QString lastDir = settings.value("lastStockfishDir", QDir::homePath()).toString();
            QString filter = "All Files (*)";
#ifdef _WIN32
            filter = "Executables (*.exe);;All Files (*)";
#endif
            QString fileName = QFileDialog::getOpenFileName(this, "Select Stockfish Executable", lastDir, filter);
            if (!fileName.isEmpty()) {
                auto* pEdit = findChild<QLineEdit*>("stockfishPathEdit");
                if (pEdit) pEdit->setText(fileName);
                settings.setValue("lastStockfishDir", QFileInfo(fileName).absolutePath());
                saveSettings();
            }
        });
    }
    
    if (auto* stockfishSearchBtn = findChild<QPushButton*>("stockfishSearchBtn")) {
        connect(stockfishSearchBtn, &QPushButton::clicked, this, [this]() {
            emit logMessage("Searching for Stockfish executable...");
            QCoreApplication::processEvents();
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
                if (QFileInfo::exists(p)) { foundPath = QFileInfo(p).absoluteFilePath(); break; }
            }
            if (foundPath.isEmpty()) {
                const QStringList candidateDirs = {
                    appDir, appDir + "/stockfish", appDir + "/../stockfish", appDir + "/../../stockfish",
                    "C:/stockfish", "C:/stockfish/stockfish", "C:/stockfish-windows-x86-64-avx2", "C:/stockfish-windows-x86-64-avx2/stockfish"
                };
                for (const QString& dirPath : candidateDirs) {
                    foundPath = findMatchingExecutable(dirPath);
                    if (!foundPath.isEmpty()) break;
                }
            }
            if (foundPath.isEmpty()) {
                QStringList baseDirs = { QDir::rootPath(), "C:/", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) };
                for (const QString& base : baseDirs) {
                    QDir dir(base);
                    if (!dir.exists()) continue;
                    QFileInfoList subdirs = dir.entryInfoList(QStringList() << "*stockfish*", QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QFileInfo& sInfo : subdirs) {
                        QDirIterator it(sInfo.absoluteFilePath(), exePatterns, QDir::Files | QDir::Executable, QDirIterator::Subdirectories);
                        if (it.hasNext()) { foundPath = it.next(); break; }
                    }
                    if (!foundPath.isEmpty()) break;
                }
            }
            if (!foundPath.isEmpty()) {
                auto* pEdit = findChild<QLineEdit*>("stockfishPathEdit");
                if (pEdit) pEdit->setText(QDir::toNativeSeparators(foundPath));
                emit logMessage("Found Stockfish at: " + QDir::toNativeSeparators(foundPath));
                saveSettings();
            } else {
                emit logMessage("Could not automatically find Stockfish. Please browse manually.");
            }
        });
    }

    if (auto* depthComboBox = findChild<QComboBox*>("depthComboBox")) connect(depthComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* timeComboBox = findChild<QComboBox*>("timeComboBox")) connect(timeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* nodesComboBox = findChild<QComboBox*>("nodesComboBox")) connect(nodesComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* analysisDepthComboBox = findChild<QComboBox*>("analysisDepthComboBox")) connect(analysisDepthComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit")) connect(stockfishPathEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    
    connect(themeComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        saveSettings();
        emit logMessage("Theme changed to: " + themeComboBox_->currentText());
        emit themeChanged();
    });
    
    if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
        connect(mat, &ToggleSwitch::toggled, this, [this](bool checked) {
            emit logMessage(checked ? "Move quality annotations enabled" : "Move quality annotations disabled");
            saveSettings();
        });
    }
    
    if (auto* videoCodecComboBox = findChild<QComboBox*>("videoCodecComboBox")) {
        auto* audioCodecComboBox = findChild<QComboBox*>("audioCodecComboBox");
        auto* extensionComboBox = findChild<QComboBox*>("extensionComboBox");
        if (audioCodecComboBox && extensionComboBox) {
            connect(videoCodecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, videoCodecComboBox, audioCodecComboBox, extensionComboBox]() {
                const QString vCodec = videoCodecComboBox->currentData().toString();
                const QString currentAudio = audioCodecComboBox->currentData().toString();
                QString currentExt = extensionComboBox->currentText();
                audioCodecComboBox->blockSignals(true); extensionComboBox->blockSignals(true);
                audioCodecComboBox->clear(); extensionComboBox->clear();
                if (vCodec == "libvpx-vp9") {
                    audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
                    audioCodecComboBox->addItem("Opus audio (recommended for WebM)", "libopus");
                    extensionComboBox->addItems({".webm", ".mkv"});
                } else {
                    audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
                    audioCodecComboBox->addItem("AAC audio (standard)", "aac");
                    extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
                }
                int aIdx = audioCodecComboBox->findData(currentAudio);
                if (aIdx >= 0) audioCodecComboBox->setCurrentIndex(aIdx);
                int eIdx = extensionComboBox->findText(currentExt);
                if (eIdx >= 0) extensionComboBox->setCurrentIndex(eIdx);
                audioCodecComboBox->blockSignals(false); extensionComboBox->blockSignals(false);
                saveSettings();
            });
        }
    }
    
    if (auto* audioCodecComboBox = findChild<QComboBox*>("audioCodecComboBox")) connect(audioCodecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* extensionComboBox = findChild<QComboBox*>("extensionComboBox")) connect(extensionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* resolutionComboBox = findChild<QComboBox*>("resolutionComboBox")) connect(resolutionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    if (auto* qualityComboBox = findChild<QComboBox*>("qualityComboBox")) connect(qualityComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(debugLevelComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
}

void SettingsDialog::loadSettings() {
    QSettings settings;
    const auto widgets = this->findChildren<QWidget*>();
    for (auto* w : widgets) w->blockSignals(true);

    pgnExportToggle_->setChecked(settings.value("generatePgn", true).toBool());
    subtitlesToggle_->setChecked(settings.value("generateSubtitles", false).toBool());
    analysisVideoToggle_->setChecked(settings.value("generateAnalysisVideo", false).toBool());
    if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
        mat->setChecked(settings.value("analysis/enableMoveAnnotations", true).toBool());
    }

    int multiPv = settings.value("multiPv", 3).toInt();
    int multiPvIdx = multiPvComboBox_->findData(multiPv);
    multiPvComboBox_->setCurrentIndex(multiPvIdx >= 0 ? multiPvIdx : 2);
    const int defaultThreads = maxHardwareThreadCount();
    setThreadComboValue(threadComboBox_, settings.value("ffmpegThreads", defaultThreads).toInt());
    
    if (auto* d = findChild<QComboBox*>("depthComboBox")) { int idx = d->findData(settings.value("stockfishDepth", 15).toInt()); d->setCurrentIndex(idx >= 0 ? idx : d->findData(15)); }
    if (auto* t = findChild<QComboBox*>("timeComboBox")) { int idx = t->findData(settings.value("stockfishTime", 0).toInt()); t->setCurrentIndex(idx >= 0 ? idx : t->findData(0)); }
    if (auto* n = findChild<QComboBox*>("nodesComboBox")) { int idx = n->findData(settings.value("stockfishNodes", 0).toInt()); n->setCurrentIndex(idx >= 0 ? idx : n->findData(0)); }
    if (auto* ad = findChild<QComboBox*>("analysisDepthComboBox")) { int idx = ad->findData(settings.value("stockfishAnalysisDepth", 5).toInt()); ad->setCurrentIndex(idx >= 0 ? idx : ad->findData(5)); }
    if (auto* p = findChild<QLineEdit*>("stockfishPathEdit")) p->setText(settings.value("stockfishPath", "").toString());
    
    if (auto* fp = findChild<ToggleSwitch*>("fastPreviewToggle")) {
        bool fpEnabled = settings.value("fastPreview", false).toBool();
        fp->setChecked(fpEnabled);
        if (auto* d = findChild<QComboBox*>("depthComboBox")) d->setEnabled(!fpEnabled);
        if (auto* t = findChild<QComboBox*>("timeComboBox")) t->setEnabled(!fpEnabled);
        if (auto* n = findChild<QComboBox*>("nodesComboBox")) n->setEnabled(!fpEnabled);
    }

    debugLevelComboBox_->setCurrentIndex(settings.value("debugLevel", 0).toInt());
    themeComboBox_->setCurrentIndex(settings.value("themeMode", 0).toInt());

    bool sameAsSource = settings.value("outSameAsSource", true).toBool();
    if (auto* ss = findChild<QRadioButton*>("sameAsSourceRadio")) ss->setChecked(sameAsSource);
    if (auto* cd = findChild<QRadioButton*>("customDirRadio")) cd->setChecked(!sameAsSource);
    if (auto* e = findChild<QLineEdit*>("customDirEdit")) e->setText(settings.value("outCustomDir", "").toString());

    if (auto* vc = findChild<QComboBox*>("videoCodecComboBox")) {
        const QString videoCodec = canonicalVideoCodec(settings.value("videoCodec", "libx264").toString());
        int vIdx = vc->findData(videoCodec);
        vc->setCurrentIndex(vIdx >= 0 ? vIdx : 0);
        
        // Ensure dependent comboboxes have the correct items for the loaded codec before setting their values
        if (auto* ac = findChild<QComboBox*>("audioCodecComboBox")) {
            if (auto* ec = findChild<QComboBox*>("extensionComboBox")) {
                ac->clear(); ec->clear();
                if (vc->currentData().toString() == "libvpx-vp9") {
                    ac->addItem("Keep original audio (fastest)", "copy");
                    ac->addItem("Opus audio (recommended for WebM)", "libopus");
                    ec->addItems({".webm", ".mkv"});
                } else {
                    ac->addItem("Keep original audio (fastest)", "copy");
                    ac->addItem("AAC audio (standard)", "aac");
                    ec->addItems({".mp4", ".mkv", ".avi", ".mov"});
                }
            }
        }
    }
    if (auto* ac = findChild<QComboBox*>("audioCodecComboBox")) {
        const QString audioCodec = canonicalAudioCodec(settings.value("audioCodec", "copy").toString());
        int aIdx = ac->findData(audioCodec);
        ac->setCurrentIndex(aIdx >= 0 ? aIdx : 0);
    }
    if (auto* ec = findChild<QComboBox*>("extensionComboBox")) {
        int eIdx = ec->findText(settings.value("videoExtension", ".mp4").toString());
        ec->setCurrentIndex(eIdx >= 0 ? eIdx : 0);
    }
    if (auto* rc = findChild<QComboBox*>("resolutionComboBox")) {
        const QString resolution = canonicalResolution(settings.value("videoResolution", "Source Resolution").toString());
        int rIdx = rc->findData(resolution);
        rc->setCurrentIndex(rIdx >= 0 ? rIdx : 0);
    }
    if (auto* q = findChild<QComboBox*>("qualityComboBox")) {
        int val = settings.value("videoQuality", 23).toInt();
        int idx = q->findData(val);
        q->setCurrentIndex(idx >= 0 ? idx : 2);
    }

    if (auto* m = findChild<QComboBox*>("memoryLimitComboBox")) { int idx = m->findData(settings.value("memoryLimitMB", 0).toInt()); m->setCurrentIndex(idx >= 0 ? idx : m->findData(0)); }

    if (auto* spin = findChild<QSpinBox*>("logRetentionSpinBox")) {
        spin->setValue(settings.value("logRetentionCount", 10).toInt());
    }

    if (auto* cb = findChild<QCheckBox*>("compressLogsCheck")) {
        cb->setChecked(settings.value("compressOldLogs", false).toBool());
    }

    if (auto* rot = findChild<ToggleSwitch*>("removeOriginalToggle")) {
        rot->setChecked(settings.value("removeOriginalVideo", false).toBool());
    }

    for (auto* w : widgets) w->blockSignals(false);
}

void SettingsDialog::saveSettings() {
    QSettings settings;
    settings.setValue("generatePgn", pgnExportToggle_->isChecked());
    settings.setValue("generateSubtitles", subtitlesToggle_->isChecked());
    settings.setValue("generateAnalysisVideo", analysisVideoToggle_->isChecked());
    settings.setValue("multiPv", multiPvComboBox_->currentData().toInt());
    settings.setValue("ffmpegThreads", threadComboBox_->currentData().toInt());
    settings.setValue("themeMode", themeComboBox_->currentIndex());
    if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
        settings.setValue("analysis/enableMoveAnnotations", mat->isChecked());
    }
    if (auto* rot = findChild<ToggleSwitch*>("removeOriginalToggle")) {
        settings.setValue("removeOriginalVideo", rot->isChecked());
    }

    if (auto* d = findChild<QComboBox*>("depthComboBox")) settings.setValue("stockfishDepth", d->currentData().toInt());
    if (auto* t = findChild<QComboBox*>("timeComboBox")) settings.setValue("stockfishTime", t->currentData().toInt());
    if (auto* n = findChild<QComboBox*>("nodesComboBox")) settings.setValue("stockfishNodes", n->currentData().toInt());
    if (auto* ad = findChild<QComboBox*>("analysisDepthComboBox")) settings.setValue("stockfishAnalysisDepth", ad->currentData().toInt());
    if (auto* p = findChild<QLineEdit*>("stockfishPathEdit")) settings.setValue("stockfishPath", p->text());
    settings.setValue("debugLevel", debugLevelComboBox_->currentIndex());

    if (auto* fp = findChild<ToggleSwitch*>("fastPreviewToggle")) {
        settings.setValue("fastPreview", fp->isChecked());
    }

    if (auto* ss = findChild<QRadioButton*>("sameAsSourceRadio")) settings.setValue("outSameAsSource", ss->isChecked());
    if (auto* cd = findChild<QLineEdit*>("customDirEdit")) settings.setValue("outCustomDir", cd->text());
    if (auto* vc = findChild<QComboBox*>("videoCodecComboBox")) settings.setValue("videoCodec", vc->currentData().toString());
    if (auto* ac = findChild<QComboBox*>("audioCodecComboBox")) settings.setValue("audioCodec", ac->currentData().toString());
    if (auto* ec = findChild<QComboBox*>("extensionComboBox")) settings.setValue("videoExtension", ec->currentText());
    if (auto* rc = findChild<QComboBox*>("resolutionComboBox")) settings.setValue("videoResolution", rc->currentData().toString());
    if (auto* q = findChild<QComboBox*>("qualityComboBox")) settings.setValue("videoQuality", q->currentData().toInt());
    
    if (auto* m = findChild<QComboBox*>("memoryLimitComboBox")) settings.setValue("memoryLimitMB", m->currentData().toInt());
    if (auto* spin = findChild<QSpinBox*>("logRetentionSpinBox")) {
        settings.setValue("logRetentionCount", spin->value());
    }
    if (auto* cb = findChild<QCheckBox*>("compressLogsCheck")) {
        settings.setValue("compressOldLogs", cb->isChecked());
    }
}

void SettingsDialog::populateSettings(ProcessingSettings& s) const {
    const bool enableMoveAnnotations = findChild<ToggleSwitch*>("moveAnnotationsToggle")
        ? findChild<ToggleSwitch*>("moveAnnotationsToggle")->isChecked()
        : true;

    s.generatePgn = pgnExportToggle_->isChecked();
    s.generateSubtitles = subtitlesToggle_->isChecked();
    s.enableMoveAnnotations = enableMoveAnnotations;
    s.enableStockfish = enableMoveAnnotations;
    s.generateAnalysisVideo = analysisVideoToggle_->isChecked() || subtitlesToggle_->isChecked();
    s.multiPv = multiPvComboBox_->currentData().toInt();
    s.ffmpegThreads = threadComboBox_->currentData().toInt();
    s.stockfishDepth = findChild<QComboBox*>("depthComboBox") ? findChild<QComboBox*>("depthComboBox")->currentData().toInt() : 15;
    s.stockfishTime = findChild<QComboBox*>("timeComboBox") ? findChild<QComboBox*>("timeComboBox")->currentData().toInt() : 0;
    s.stockfishNodes = findChild<QComboBox*>("nodesComboBox") ? findChild<QComboBox*>("nodesComboBox")->currentData().toInt() : 0;
    s.stockfishAnalysisDepth = findChild<QComboBox*>("analysisDepthComboBox") ? findChild<QComboBox*>("analysisDepthComboBox")->currentData().toInt() : 5;
    s.stockfishPath = findChild<QLineEdit*>("stockfishPathEdit") ? findChild<QLineEdit*>("stockfishPathEdit")->text() : "";
    s.debugLevel = debugLevelComboBox_->currentIndex();
    s.memoryLimitMB = findChild<QComboBox*>("memoryLimitComboBox") ? findChild<QComboBox*>("memoryLimitComboBox")->currentData().toInt() : 0;

    if (auto* fp = findChild<ToggleSwitch*>("fastPreviewToggle")) {
        if (fp->isChecked()) {
            s.stockfishDepth = 10;
            s.stockfishTime = 2; // 2 seconds max
            s.stockfishNodes = 0;
        }
    }
}

void SettingsDialog::applySettingsToUi(const ProcessingSettings& settings) {
    pgnExportToggle_->setChecked(settings.generatePgn);
    subtitlesToggle_->setChecked(settings.generateSubtitles);
    if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
        mat->setChecked(settings.enableMoveAnnotations);
    }
    analysisVideoToggle_->setChecked(settings.generateAnalysisVideo);
    int idx = multiPvComboBox_->findData(settings.multiPv);
    if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);
    setThreadComboValue(threadComboBox_, settings.ffmpegThreads);
    if (auto* d = findChild<QComboBox*>("depthComboBox")) { int idx = d->findData(settings.stockfishDepth); if (idx >= 0) d->setCurrentIndex(idx); }
    if (auto* t = findChild<QComboBox*>("timeComboBox")) { int idx = t->findData(settings.stockfishTime); if (idx >= 0) t->setCurrentIndex(idx); }
    if (auto* n = findChild<QComboBox*>("nodesComboBox")) { int idx = n->findData(settings.stockfishNodes); if (idx >= 0) n->setCurrentIndex(idx); }
    if (auto* ad = findChild<QComboBox*>("analysisDepthComboBox")) { int idx = ad->findData(settings.stockfishAnalysisDepth); if (idx >= 0) ad->setCurrentIndex(idx); }
    if (auto* p = findChild<QLineEdit*>("stockfishPathEdit")) p->setText(settings.stockfishPath);
    debugLevelComboBox_->setCurrentIndex(settings.debugLevel);
}

void SettingsDialog::applyHeadlessOverrides(int pgnOverride, int analysisVideoOverride, int moveLabelsOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, int memoryLimit) {
    if (pgnOverride != -1) pgnExportToggle_->setChecked(pgnOverride != 0);
    if (analysisVideoOverride != -1) analysisVideoToggle_->setChecked(analysisVideoOverride != 0);
    if (moveLabelsOverride != -1) {
        if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
            mat->setChecked(moveLabelsOverride != 0);
        }
    }
    if (multiPv > 0) {
        int idx = multiPvComboBox_->findData(multiPv);
        if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);
    }
    if (threads > 0) setThreadComboValue(threadComboBox_, threads);
    if (depth > 0) { if (auto* d = findChild<QComboBox*>("depthComboBox")) { int idx = d->findData(depth); if (idx >= 0) d->setCurrentIndex(idx); } }
    if (time >= 0) { if (auto* t = findChild<QComboBox*>("timeComboBox")) { int idx = t->findData(time); if (idx >= 0) t->setCurrentIndex(idx); } }
    if (nodes >= 0) { if (auto* n = findChild<QComboBox*>("nodesComboBox")) { int idx = n->findData(nodes); if (idx >= 0) n->setCurrentIndex(idx); } }
    if (analysisDepth > 0) { if (auto* ad = findChild<QComboBox*>("analysisDepthComboBox")) { int idx = ad->findData(analysisDepth); if (idx >= 0) ad->setCurrentIndex(idx); } }
    if (memoryLimit >= 0) { if (auto* m = findChild<QComboBox*>("memoryLimitComboBox")) { int idx = m->findData(memoryLimit); if (idx >= 0) m->setCurrentIndex(idx); } }
    if (!debugLevelStr.isEmpty() && debugLevelComboBox_) {
        if (debugLevelStr == "NONE") debugLevelComboBox_->setCurrentIndex(0);
        else if (debugLevelStr == "MOVES") debugLevelComboBox_->setCurrentIndex(1);
        else if (debugLevelStr == "FULL") debugLevelComboBox_->setCurrentIndex(2);
    }
}

} // namespace cta
