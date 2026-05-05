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
} // namespace

namespace cta {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Settings");
    resize(650, 500);
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

    auto* outputDirGroup = new QGroupBox("Output Directory");
    outputDirGroup->setToolTip("Choose where the generated output files will be saved");
    auto* outputDirLayout = new QVBoxLayout(outputDirGroup);

    auto* sameAsSourceRadio = new QRadioButton("Save to same folder as source video");
    sameAsSourceRadio->setObjectName("sameAsSourceRadio");
    sameAsSourceRadio->setChecked(true);
    sameAsSourceRadio->setToolTip("Save output files in the same directory as the input video file");
    outputDirLayout->addWidget(sameAsSourceRadio);

    auto* customDirHLayout = new QHBoxLayout();
    auto* customDirRadio = new QRadioButton("Custom directory:");
    customDirRadio->setObjectName("customDirRadio");
    customDirRadio->setToolTip("Save output files in a custom specified directory");
    customDirHLayout->addWidget(customDirRadio);

    auto* customDirEdit = new QLineEdit();
    customDirEdit->setObjectName("customDirEdit");
    customDirEdit->setEnabled(false);
    customDirEdit->setToolTip("Path to the custom output directory");
    customDirHLayout->addWidget(customDirEdit);

    auto* customDirBtn = new QPushButton("Browse...");
    customDirBtn->setObjectName("customDirBtn");
    customDirBtn->setEnabled(false);
    customDirBtn->setToolTip("Browse for a custom output directory");
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
    auto* togglesGroup = new QGroupBox("Generation Options");
    togglesGroup->setToolTip("Enable or disable specific output files");
    auto* togglesLayout = new QVBoxLayout(togglesGroup);
    togglesLayout->addWidget(createToggleRow("Generate Game Moves File (PGN)", "Exports the extracted moves to a PGN file", pgnExportToggle_, true));
    togglesLayout->addWidget(createToggleRow("Generate PGN with Stockfish Analysis", "Generates a PGN file with Stockfish evaluations", stockfishToggle_, false));

    ToggleSwitch* moveAnnotationsToggle = nullptr;
    auto* annotationsRow = createToggleRow("Generate Move Quality Annotations", "Adds chess.com style move symbols (!!, ?, etc.) and Book tags to the PGN and Video", moveAnnotationsToggle, true);
    moveAnnotationsToggle->setObjectName("moveAnnotationsToggle");
    togglesLayout->addWidget(annotationsRow);

    togglesLayout->addWidget(createToggleRow("Generate Analysis Video", "Generates an Analysis Video showing the board", analysisVideoToggle_, false));
    generalLayout->addWidget(togglesGroup);

    // Theme selector
    auto* themeGroup = new QGroupBox("Appearance");
    themeGroup->setToolTip("Customize the look and feel of the application");
    auto* themeLayout = new QHBoxLayout(themeGroup);
    auto* themeLabel = new QLabel("Theme:");
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
    auto* encodingGroup = new QGroupBox("Video Export Settings");
    encodingGroup->setToolTip("Configure settings for the generated analysis video");
    auto* encodingLayout = new QVBoxLayout(encodingGroup);

    auto* videoCodecLayout = new QHBoxLayout();
    videoCodecLayout->addWidget(new QLabel("Video Format:"));
    auto* videoCodecComboBox = new QComboBox();
    videoCodecComboBox->setObjectName("videoCodecComboBox");
    videoCodecComboBox->addItems({
        "libx264 (H.264 - Fast & Compatible)", 
        "h264_nvenc (NVIDIA GPU H.264 - Fastest)", 
        "libx265 (HEVC - High Quality)", 
        "hevc_nvenc (NVIDIA GPU HEVC - Fastest)", 
        "libvpx-vp9 (VP9 - Web Optimized)"
    });
    videoCodecComboBox->setProperty("class", "dropdown");
    videoCodecComboBox->setToolTip("Select the video compression format. H.264 is recommended for best compatibility.");
    videoCodecLayout->addWidget(videoCodecComboBox);
    videoCodecLayout->addStretch();
    encodingLayout->addLayout(videoCodecLayout);

    auto* audioCodecLayout = new QHBoxLayout();
    audioCodecLayout->addWidget(new QLabel("Audio Track:"));
    auto* audioCodecComboBox = new QComboBox();
    audioCodecComboBox->setObjectName("audioCodecComboBox");
    audioCodecComboBox->addItems({"copy (Original, Fastest)", "aac (Standard)"});
    audioCodecComboBox->setProperty("class", "dropdown");
    audioCodecComboBox->setToolTip("Select the audio format. 'copy' retains original audio without re-encoding.");
    audioCodecLayout->addWidget(audioCodecComboBox);
    audioCodecLayout->addStretch();
    encodingLayout->addLayout(audioCodecLayout);

    auto* extensionLayout = new QHBoxLayout();
    extensionLayout->addWidget(new QLabel("File Type:"));
    auto* extensionComboBox = new QComboBox();
    extensionComboBox->setObjectName("extensionComboBox");
    extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
    extensionComboBox->setProperty("class", "dropdown");
    extensionComboBox->setToolTip("Select the container format for the output video file.");
    extensionLayout->addWidget(extensionComboBox);
    extensionLayout->addStretch();
    encodingLayout->addLayout(extensionLayout);

    auto* resolutionLayout = new QHBoxLayout();
    resolutionLayout->addWidget(new QLabel("Output Resolution:"));
    auto* resolutionComboBox = new QComboBox();
    resolutionComboBox->setObjectName("resolutionComboBox");
    resolutionComboBox->addItems({"Source Resolution (No Scaling)", "4K (3840x2160)", "1080p (1920x1080)", "720p (1280x720)"});
    resolutionComboBox->setProperty("class", "dropdown");
    resolutionComboBox->setToolTip("Select the output video resolution. Scaling down can save processing time and space.");
    resolutionLayout->addWidget(resolutionComboBox);
    resolutionLayout->addStretch();
    encodingLayout->addLayout(resolutionLayout);

    auto* qualityLayout = new QHBoxLayout();
    qualityLayout->addWidget(new QLabel("Video Compression (CRF):"));
    auto* qualityComboBox = new QComboBox();
    qualityComboBox->setObjectName("qualityComboBox");
    qualityComboBox->addItem("Very Low Compression (CRF 18 - Best Quality)", 18);
    qualityComboBox->addItem("Low Compression (CRF 20 - High Quality)", 20);
    qualityComboBox->addItem("Standard (CRF 23 - Recommended Balance)", 23);
    qualityComboBox->addItem("High Compression (CRF 28 - Smaller File)", 28);
    qualityComboBox->addItem("Highest Compression (CRF 35 - Lowest Quality)", 35);
    qualityComboBox->setProperty("class", "dropdown");
    qualityComboBox->setToolTip("Controls the Constant Rate Factor (CRF) for FFmpeg video compression.\nLower values (18-20) produce larger, near-lossless files.\nStandard (23) provides a great sweet spot.\nHigher values (28-35) aggressively compress the video to save hard drive space, but will introduce noticeable blurriness and blocky artifacts.");
    qualityLayout->addWidget(qualityComboBox);
    qualityLayout->addStretch();
    encodingLayout->addLayout(qualityLayout);

    videoExportLayout->addWidget(encodingGroup);
    videoExportLayout->addStretch();
    tabWidget->addTab(videoExportTab, "Video Export");

    // === Tab 3: Stockfish ===
    auto* stockfishTab = new QWidget();
    auto* stockfishLayout = new QVBoxLayout(stockfishTab);
    stockfishSettingsGroup_ = new QGroupBox("Stockfish Analysis Settings");
    stockfishSettingsGroup_->setToolTip("Choose how much engine analysis to add to PGNs and analysis videos");
    auto* stockfishOptionsLayout = new QVBoxLayout(stockfishSettingsGroup_);

    stockfishOptionsLayout->addWidget(createHelpText(
        "Recommended starting point: use 1-2 lines, depth 15, and leave time/nodes unlimited. Raise depth or lines only if you want stronger analysis and do not mind waiting longer.",
        "Beginner guidance for balancing Stockfish strength against processing time"
    ));

    auto* stockfishPathLayout = new QHBoxLayout();
    stockfishPathLayout->addWidget(createSettingsLabel(
        "Stockfish App:",
        "The Stockfish executable file ChessTube Analyzer will run for engine analysis"
    ));
    auto* stockfishPathEdit = new QLineEdit();
    stockfishPathEdit->setObjectName("stockfishPathEdit");
    stockfishPathEdit->setToolTip("Path to the Stockfish engine app. Leave blank only if Stockfish is bundled or already discoverable.");
    stockfishPathLayout->addWidget(stockfishPathEdit);
    auto* stockfishPathBtn = new QPushButton("Browse...");
    stockfishPathBtn->setToolTip("Choose the Stockfish executable file manually");
    stockfishPathLayout->addWidget(stockfishPathBtn);
    auto* stockfishSearchBtn = new QPushButton("Auto-Find");
    stockfishSearchBtn->setToolTip("Search common folders for a Stockfish executable");
    stockfishPathLayout->addWidget(stockfishSearchBtn);
    stockfishOptionsLayout->addLayout(stockfishPathLayout);

    auto* multiPvLayout = new QHBoxLayout();
    multiPvLayout->addWidget(createSettingsLabel(
        "Best Moves to Show:",
        "How many candidate moves Stockfish should show for each position"
    ));
    multiPvComboBox_ = new QComboBox();
    multiPvComboBox_->addItem("1 best move (fastest, easiest to read)", 1);
    multiPvComboBox_->addItem("2 best moves (good for comparison)", 2);
    multiPvComboBox_->addItem("3 best moves (more detail)", 3);
    multiPvComboBox_->addItem("4 best moves (slowest)", 4);
    multiPvComboBox_->setProperty("class", "dropdown");
    multiPvComboBox_->setToolTip("More best moves means richer analysis, but each position takes longer to process.");
    multiPvLayout->addWidget(multiPvComboBox_);
    multiPvLayout->addStretch();
    stockfishOptionsLayout->addLayout(multiPvLayout);

    auto* depthLayout = new QHBoxLayout();
    depthLayout->addWidget(createSettingsLabel(
        "Thinking Depth:",
        "How many half-moves ahead Stockfish tries to calculate"
    ));
    auto* depthSpinBox = new QSpinBox();
    depthSpinBox->setObjectName("depthSpinBox");
    depthSpinBox->setRange(1, 40);
    depthSpinBox->setSuffix(" plies");
    depthSpinBox->setToolTip("Higher depth is usually stronger, but slower. Try 15 for normal use; 20+ is better for fast CPUs.");
    depthLayout->addWidget(depthSpinBox);
    depthLayout->addWidget(createSettingsLabel(
        "15 normal, 20+ strong",
        "A simple rule of thumb for choosing Stockfish depth"
    ));
    depthLayout->addStretch();
    stockfishOptionsLayout->addLayout(depthLayout);
    
    auto* timeLayout = new QHBoxLayout();
    timeLayout->addWidget(createSettingsLabel(
        "Time Limit per Position:",
        "Optional cap on how long Stockfish may spend on each board position"
    ));
    auto* timeSpinBox = new QSpinBox();
    timeSpinBox->setObjectName("timeSpinBox");
    timeSpinBox->setRange(0, 600); // Up to 10 minutes
    timeSpinBox->setSingleStep(1);
    timeSpinBox->setSpecialValueText("No time limit");
    timeSpinBox->setSuffix(" s");
    timeSpinBox->setToolTip("Optional safety cap in seconds. 0 means Stockfish stops by depth instead of by time.");
    timeLayout->addWidget(timeSpinBox);
    timeLayout->addStretch();
    stockfishOptionsLayout->addLayout(timeLayout);

    auto* nodesLayout = new QHBoxLayout();
    nodesLayout->addWidget(createSettingsLabel(
        "Position Limit:",
        "Optional cap on how many possible positions Stockfish may examine"
    ));
    auto* nodesSpinBox = new QSpinBox();
    nodesSpinBox->setObjectName("nodesSpinBox");
    nodesSpinBox->setRange(0, 1000000000);
    nodesSpinBox->setSingleStep(100000);
    nodesSpinBox->setSpecialValueText("No position limit");
    nodesSpinBox->setSuffix(" nodes");
    nodesSpinBox->setToolTip("Advanced limit on Stockfish's search work. Most users should leave this at 0.");
    nodesLayout->addWidget(nodesSpinBox);
    nodesLayout->addStretch();
    stockfishOptionsLayout->addLayout(nodesLayout);

    auto* analysisDepthLayout = new QHBoxLayout();
    analysisDepthLayout->addWidget(createSettingsLabel(
        "Moves to Write in Each Line:",
        "How many moves of each suggested Stockfish continuation should appear in the PGN/video text"
    ));
    auto* analysisDepthSpinBox = new QSpinBox();
    analysisDepthSpinBox->setObjectName("analysisDepthSpinBox");
    analysisDepthSpinBox->setRange(1, 10);
    analysisDepthSpinBox->setSuffix(" moves");
    analysisDepthSpinBox->setToolTip("Controls how long each displayed engine line is. Shorter lines are easier to read.");
    analysisDepthLayout->addWidget(analysisDepthSpinBox);
    analysisDepthLayout->addStretch();
    stockfishOptionsLayout->addLayout(analysisDepthLayout);

    stockfishLayout->addWidget(stockfishSettingsGroup_);
    stockfishLayout->addStretch();
    tabWidget->addTab(stockfishTab, "Stockfish");

    // === Tab 4: Advanced ===
    auto* advancedTab = new QWidget();
    auto* advancedLayout = new QVBoxLayout(advancedTab);
    auto* advancedGroup = new QGroupBox("Performance & Debugging");
    advancedGroup->setToolTip("Configure advanced performance and developer settings");
    auto* advancedGroupLayout = new QVBoxLayout(advancedGroup);

    auto* threadLayout = new QHBoxLayout();
    threadLayout->addWidget(new QLabel("FFmpeg Decode Threads:"));
    threadSpinBox_ = new QSpinBox();
    threadSpinBox_->setRange(1, 16);
    threadSpinBox_->setToolTip("Set the number of CPU threads allocated for video decoding. Higher values increase speed but use more memory.");
    threadLayout->addWidget(threadSpinBox_);
    threadLayout->addStretch();
    advancedGroupLayout->addLayout(threadLayout);

    auto* debugLevelLayout = new QHBoxLayout();
    debugLevelLayout->addWidget(new QLabel("Debug Image Generation:"));
    debugLevelComboBox_ = new QComboBox();
    debugLevelComboBox_->addItems({"None", "Moves Only", "Full"});
    debugLevelComboBox_->setProperty("class", "dropdown");
    debugLevelComboBox_->setToolTip("Select the level of debug images to save to the disk during processing.");
    debugLevelLayout->addWidget(debugLevelComboBox_);
    debugLevelLayout->addStretch();
    advancedGroupLayout->addLayout(debugLevelLayout);

    auto* memoryLimitLayout = new QHBoxLayout();
    memoryLimitLayout->addWidget(new QLabel("Memory Limit (MB):"));
    auto* memoryLimitSpinBox = new QSpinBox();
    memoryLimitSpinBox->setObjectName("memoryLimitSpinBox");
    memoryLimitSpinBox->setRange(0, 65536);
    memoryLimitSpinBox->setSingleStep(512);
    memoryLimitSpinBox->setToolTip("Limit the RAM usage of the frame prefetcher in MB. Use 0 for unlimited memory.");
    memoryLimitLayout->addWidget(memoryLimitSpinBox);
    memoryLimitLayout->addStretch();
    advancedGroupLayout->addLayout(memoryLimitLayout);

    advancedLayout->addWidget(advancedGroup);
    advancedLayout->addStretch();
    tabWidget->addTab(advancedTab, "Advanced");

    auto* dialogBtnBox = new QDialogButtonBox(QDialogButtonBox::Close);
    dialogBtnBox->setToolTip("Close the settings window and apply changes");
    connect(dialogBtnBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    dialogLayout->addWidget(dialogBtnBox);

    // Connect signals
    connect(pgnExportToggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "PGN export enabled" : "PGN export disabled");
        saveSettings();
    });
    connect(stockfishToggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
        emit logMessage(checked ? "PGN with Stockfish analysis enabled" : "PGN with Stockfish analysis disabled");
        saveSettings();
    });
    connect(analysisVideoToggle_, &ToggleSwitch::toggled, this, [this](bool checked) { 
        emit logMessage(checked ? "Analysis Video generation enabled" : "Analysis Video generation disabled");
        saveSettings(); 
    });
    connect(threadSpinBox_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(multiPvComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
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
            if (!dir.exists()) {
                return {};
            }

            for (const QString& pattern : exePatterns) {
                const QFileInfoList files = dir.entryInfoList(
                    QStringList() << pattern,
                    QDir::Files | QDir::Executable | QDir::NoSymLinks
                );
                if (!files.isEmpty()) {
                    return files.first().absoluteFilePath();
                }
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
                appDir,
                appDir + "/stockfish",
                appDir + "/../stockfish",
                appDir + "/../../stockfish",
                "C:/stockfish",
                "C:/stockfish/stockfish",
                "C:/stockfish-windows-x86-64-avx2",
                "C:/stockfish-windows-x86-64-avx2/stockfish"
            };
            for (const QString& dirPath : candidateDirs) {
                foundPath = findMatchingExecutable(dirPath);
                if (!foundPath.isEmpty()) {
                    break;
                }
            }
        }
        if (foundPath.isEmpty()) {
            QStringList baseDirs = {
                QDir::rootPath(),
                "C:/",
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            };
            for (const QString& base : baseDirs) {
                QDir dir(base);
                if (!dir.exists()) continue;
                QFileInfoList subdirs = dir.entryInfoList(QStringList() << "*stockfish*", QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QFileInfo& sInfo : subdirs) {
                    QDirIterator it(sInfo.absoluteFilePath(), exePatterns, QDir::Files | QDir::Executable, QDirIterator::Subdirectories);
                    if (it.hasNext()) {
                        foundPath = it.next();
                        break;
                    }
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
    connect(depthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(timeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(nodesSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(analysisDepthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(stockfishPathEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
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
    connect(videoCodecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, videoCodecComboBox, audioCodecComboBox, extensionComboBox]() {
        QString vCodec = videoCodecComboBox->currentText();
        QString currentAudio = audioCodecComboBox->currentText();
        QString currentExt = extensionComboBox->currentText();
        audioCodecComboBox->blockSignals(true); extensionComboBox->blockSignals(true);
        audioCodecComboBox->clear(); extensionComboBox->clear();
        if (vCodec.contains("VP9")) {
            audioCodecComboBox->addItems({"copy (Original, Fastest)", "libopus (High Quality)"});
            extensionComboBox->addItems({".webm", ".mkv"});
        } else {
            audioCodecComboBox->addItems({"copy (Original, Fastest)", "aac (Standard)"});
            extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
        }
        int aIdx = audioCodecComboBox->findText(currentAudio);
        if (aIdx >= 0) audioCodecComboBox->setCurrentIndex(aIdx);
        int eIdx = extensionComboBox->findText(currentExt);
        if (eIdx >= 0) extensionComboBox->setCurrentIndex(eIdx);
        audioCodecComboBox->blockSignals(false); extensionComboBox->blockSignals(false);
        saveSettings();
    });
    connect(audioCodecComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(extensionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(resolutionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(qualityComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    connect(debugLevelComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
}

void SettingsDialog::loadSettings() {
    QSettings settings;
    const auto widgets = this->findChildren<QWidget*>();
    for (auto* w : widgets) w->blockSignals(true);

    pgnExportToggle_->setChecked(settings.value("generatePgn", true).toBool());
    stockfishToggle_->setChecked(settings.value("enableStockfish", false).toBool());
    analysisVideoToggle_->setChecked(settings.value("generateAnalysisVideo", false).toBool());
    if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
        mat->setChecked(settings.value("analysis/enableMoveAnnotations", true).toBool());
    }

    int multiPv = settings.value("multiPv", 3).toInt();
    int multiPvIdx = multiPvComboBox_->findData(multiPv);
    multiPvComboBox_->setCurrentIndex(multiPvIdx >= 0 ? multiPvIdx : 2);
    threadSpinBox_->setValue(settings.value("ffmpegThreads", 4).toInt());
    
    if (auto* d = findChild<QSpinBox*>("depthSpinBox")) d->setValue(settings.value("stockfishDepth", 15).toInt());
    if (auto* t = findChild<QSpinBox*>("timeSpinBox")) t->setValue(settings.value("stockfishTime", 0).toInt());
    if (auto* n = findChild<QSpinBox*>("nodesSpinBox")) n->setValue(settings.value("stockfishNodes", 0).toInt());
    if (auto* ad = findChild<QSpinBox*>("analysisDepthSpinBox")) ad->setValue(settings.value("stockfishAnalysisDepth", 5).toInt());
    if (auto* p = findChild<QLineEdit*>("stockfishPathEdit")) p->setText(settings.value("stockfishPath", "").toString());
    
    debugLevelComboBox_->setCurrentIndex(settings.value("debugLevel", 0).toInt());
    themeComboBox_->setCurrentIndex(settings.value("themeMode", 0).toInt());

    bool sameAsSource = settings.value("outSameAsSource", true).toBool();
    if (auto* ss = findChild<QRadioButton*>("sameAsSourceRadio")) ss->setChecked(sameAsSource);
    if (auto* cd = findChild<QRadioButton*>("customDirRadio")) cd->setChecked(!sameAsSource);
    if (auto* e = findChild<QLineEdit*>("customDirEdit")) e->setText(settings.value("outCustomDir", "").toString());

    if (auto* vc = findChild<QComboBox*>("videoCodecComboBox")) {
        int vIdx = vc->findText(settings.value("videoCodec", "libx264 (H.264 - Fast & Compatible)").toString());
        vc->setCurrentIndex(vIdx >= 0 ? vIdx : 0);
        
        // Ensure dependent comboboxes have the correct items for the loaded codec before setting their values
        if (auto* ac = findChild<QComboBox*>("audioCodecComboBox")) {
            if (auto* ec = findChild<QComboBox*>("extensionComboBox")) {
                ac->clear(); ec->clear();
                if (vc->currentText().contains("VP9")) {
                    ac->addItems({"copy (Original, Fastest)", "libopus (High Quality)"});
                    ec->addItems({".webm", ".mkv"});
                } else {
                    ac->addItems({"copy (Original, Fastest)", "aac (Standard)"});
                    ec->addItems({".mp4", ".mkv", ".avi", ".mov"});
                }
            }
        }
    }
    if (auto* ac = findChild<QComboBox*>("audioCodecComboBox")) {
        int aIdx = ac->findText(settings.value("audioCodec", "copy (Original, Fastest)").toString());
        ac->setCurrentIndex(aIdx >= 0 ? aIdx : 0);
    }
    if (auto* ec = findChild<QComboBox*>("extensionComboBox")) {
        int eIdx = ec->findText(settings.value("videoExtension", ".mp4").toString());
        ec->setCurrentIndex(eIdx >= 0 ? eIdx : 0);
    }
    if (auto* rc = findChild<QComboBox*>("resolutionComboBox")) {
        int rIdx = rc->findText(settings.value("videoResolution", "Source Resolution (No Scaling)").toString());
        rc->setCurrentIndex(rIdx >= 0 ? rIdx : 0);
    }
    if (auto* q = findChild<QComboBox*>("qualityComboBox")) {
        int val = settings.value("videoQuality", 23).toInt();
        int idx = q->findData(val);
        q->setCurrentIndex(idx >= 0 ? idx : 2);
    }

    if (auto* m = findChild<QSpinBox*>("memoryLimitSpinBox")) m->setValue(settings.value("memoryLimitMB", 0).toInt());

    for (auto* w : widgets) w->blockSignals(false);
}

void SettingsDialog::saveSettings() {
    QSettings settings;
    settings.setValue("generatePgn", pgnExportToggle_->isChecked());
    settings.setValue("enableStockfish", stockfishToggle_->isChecked());
    settings.setValue("generateAnalysisVideo", analysisVideoToggle_->isChecked());
    settings.setValue("multiPv", multiPvComboBox_->currentData().toInt());
    settings.setValue("ffmpegThreads", threadSpinBox_->value());
    settings.setValue("themeMode", themeComboBox_->currentIndex());
    if (auto* mat = findChild<ToggleSwitch*>("moveAnnotationsToggle")) {
        settings.setValue("analysis/enableMoveAnnotations", mat->isChecked());
    }

    if (auto* d = findChild<QSpinBox*>("depthSpinBox")) settings.setValue("stockfishDepth", d->value());
    if (auto* t = findChild<QSpinBox*>("timeSpinBox")) settings.setValue("stockfishTime", t->value());
    if (auto* n = findChild<QSpinBox*>("nodesSpinBox")) settings.setValue("stockfishNodes", n->value());
    if (auto* ad = findChild<QSpinBox*>("analysisDepthSpinBox")) settings.setValue("stockfishAnalysisDepth", ad->value());
    if (auto* p = findChild<QLineEdit*>("stockfishPathEdit")) settings.setValue("stockfishPath", p->text());
    settings.setValue("debugLevel", debugLevelComboBox_->currentIndex());

    if (auto* ss = findChild<QRadioButton*>("sameAsSourceRadio")) settings.setValue("outSameAsSource", ss->isChecked());
    if (auto* cd = findChild<QLineEdit*>("customDirEdit")) settings.setValue("outCustomDir", cd->text());
    if (auto* vc = findChild<QComboBox*>("videoCodecComboBox")) settings.setValue("videoCodec", vc->currentText());
    if (auto* ac = findChild<QComboBox*>("audioCodecComboBox")) settings.setValue("audioCodec", ac->currentText());
    if (auto* ec = findChild<QComboBox*>("extensionComboBox")) settings.setValue("videoExtension", ec->currentText());
    if (auto* rc = findChild<QComboBox*>("resolutionComboBox")) settings.setValue("videoResolution", rc->currentText());
    if (auto* q = findChild<QComboBox*>("qualityComboBox")) settings.setValue("videoQuality", q->currentData().toInt());
    
    if (auto* m = findChild<QSpinBox*>("memoryLimitSpinBox")) settings.setValue("memoryLimitMB", m->value());
}

void SettingsDialog::populateSettings(ProcessingSettings& s) const {
    s.generatePgn = pgnExportToggle_->isChecked() || stockfishToggle_->isChecked();
    s.enableStockfish = stockfishToggle_->isChecked();
    s.generateAnalysisVideo = analysisVideoToggle_->isChecked();
    s.multiPv = multiPvComboBox_->currentData().toInt();
    s.ffmpegThreads = threadSpinBox_->value();
    s.stockfishDepth = findChild<QSpinBox*>("depthSpinBox") ? findChild<QSpinBox*>("depthSpinBox")->value() : 15;
    s.stockfishTime = findChild<QSpinBox*>("timeSpinBox") ? findChild<QSpinBox*>("timeSpinBox")->value() : 0;
    s.stockfishNodes = findChild<QSpinBox*>("nodesSpinBox") ? findChild<QSpinBox*>("nodesSpinBox")->value() : 0;
    s.stockfishAnalysisDepth = findChild<QSpinBox*>("analysisDepthSpinBox") ? findChild<QSpinBox*>("analysisDepthSpinBox")->value() : 5;
    s.stockfishPath = findChild<QLineEdit*>("stockfishPathEdit") ? findChild<QLineEdit*>("stockfishPathEdit")->text() : "";
    s.debugLevel = debugLevelComboBox_->currentIndex();
    s.memoryLimitMB = findChild<QSpinBox*>("memoryLimitSpinBox") ? findChild<QSpinBox*>("memoryLimitSpinBox")->value() : 0;
}

void SettingsDialog::applySettingsToUi(const ProcessingSettings& settings) {
    pgnExportToggle_->setChecked(settings.generatePgn);
    stockfishToggle_->setChecked(settings.enableStockfish);
    analysisVideoToggle_->setChecked(settings.generateAnalysisVideo);
    int idx = multiPvComboBox_->findData(settings.multiPv);
    if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);
    threadSpinBox_->setValue(settings.ffmpegThreads);
    if (auto* d = findChild<QSpinBox*>("depthSpinBox")) d->setValue(settings.stockfishDepth);
    if (auto* t = findChild<QSpinBox*>("timeSpinBox")) t->setValue(settings.stockfishTime);
    if (auto* n = findChild<QSpinBox*>("nodesSpinBox")) n->setValue(settings.stockfishNodes);
    if (auto* ad = findChild<QSpinBox*>("analysisDepthSpinBox")) ad->setValue(settings.stockfishAnalysisDepth);
    if (auto* p = findChild<QLineEdit*>("stockfishPathEdit")) p->setText(settings.stockfishPath);
    debugLevelComboBox_->setCurrentIndex(settings.debugLevel);
}

void SettingsDialog::applyHeadlessOverrides(int pgnOverride, int stockfishOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, int memoryLimit) {
    if (pgnOverride != -1) pgnExportToggle_->setChecked(pgnOverride != 0);
    if (stockfishOverride != -1) stockfishToggle_->setChecked(stockfishOverride != 0);
    if (multiPv > 0) {
        int idx = multiPvComboBox_->findData(multiPv);
        if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);
    }
    if (threads > 0) threadSpinBox_->setValue(threads);
    if (depth > 0) { if (auto* d = findChild<QSpinBox*>("depthSpinBox")) d->setValue(depth); }
    if (time >= 0) { if (auto* t = findChild<QSpinBox*>("timeSpinBox")) t->setValue(time); }
    if (nodes >= 0) { if (auto* n = findChild<QSpinBox*>("nodesSpinBox")) n->setValue(nodes); }
    if (analysisDepth > 0) { if (auto* ad = findChild<QSpinBox*>("analysisDepthSpinBox")) ad->setValue(analysisDepth); }
    if (memoryLimit >= 0) { if (auto* m = findChild<QSpinBox*>("memoryLimitSpinBox")) m->setValue(memoryLimit); }
    if (!debugLevelStr.isEmpty() && debugLevelComboBox_) {
        if (debugLevelStr == "NONE") debugLevelComboBox_->setCurrentIndex(0);
        else if (debugLevelStr == "MOVES") debugLevelComboBox_->setCurrentIndex(1);
        else if (debugLevelStr == "FULL") debugLevelComboBox_->setCurrentIndex(2);
    }
}

} // namespace cta
