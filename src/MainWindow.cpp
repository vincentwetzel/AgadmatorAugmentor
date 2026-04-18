// Extracted from cpp directory
#include "MainWindow.h"
#include "VideoProcessorWorker.h"
#include "ToggleSwitch.h"
#include "ThemeManager.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QRadioButton>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QProgressBar>
#include <QSpinBox>
#include <QComboBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QSizePolicy>
#include <QSettings>
#include <QEventLoop>
#include <QMetaMethod>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDirIterator>
#include <QToolButton>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSize>
#include <QtMath>

#ifdef _WIN32
#include <stdlib.h>
#endif

static void set_ffmpeg_threads(int threads) {
    std::string val = std::to_string(threads);
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_THREADS", val.c_str());
#else
    setenv("OPENCV_FFMPEG_THREADS", val.c_str(), 1);
#endif
}

static QWidget* createToggleRow(const QString& label, const QString& tooltip, ToggleSwitch*& outToggle, bool checked = false) {
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

static QIcon createSettingsCogIcon(const QColor& color) {
    constexpr int canvasSize = 96;
    constexpr qreal center = canvasSize / 2.0;
    constexpr qreal bodyRadius = 24.0;
    constexpr qreal ringCutoutRadius = 14.0;
    constexpr qreal hubRadius = 4.75;
    constexpr qreal toothCenterRadius = 28.5;
    constexpr qreal toothWidth = 8.5;
    constexpr qreal toothHeight = 15.0;
    constexpr qreal toothCornerRadius = 4.0;

    QPixmap pixmap(canvasSize, canvasSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    QPainterPath gearPath;
    gearPath.addEllipse(QPointF(center, center), bodyRadius, bodyRadius);

    for (int i = 0; i < 8; ++i) {
        painter.save();
        painter.translate(center, center);
        painter.rotate(i * 45.0);
        QPainterPath toothPath;
        toothPath.addRoundedRect(
            QRectF(-toothWidth / 2.0, -toothCenterRadius - toothHeight / 2.0, toothWidth, toothHeight),
            toothCornerRadius,
            toothCornerRadius
        );
        gearPath.addPath(painter.transform().map(toothPath));
        painter.restore();
    }

    QColor fillColor = color;
    fillColor.setAlpha(242);
    painter.setBrush(fillColor);
    painter.drawPath(gearPath.simplified());

    QColor rimColor = color.lighter(112);
    rimColor.setAlpha(80);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(rimColor, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QPointF(center, center), bodyRadius - 1.5, bodyRadius - 1.5);

    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::transparent);
    painter.drawEllipse(QPointF(center, center), ringCutoutRadius, ringCutoutRadius);
    painter.drawEllipse(QPointF(center, center), hubRadius, hubRadius);

    return QIcon(pixmap);
}

namespace aa {

const char* MainWindow::SETTINGS_ORG = "ChessTubeAnalyzer";
const char* MainWindow::SETTINGS_APP = "ChessTubeAnalyzer";

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("ChessTube Analyzer");
    resize(800, 600);

    setupUi();
    setupWorker();
    loadSettings(); // Load all settings from INI file
    ensureStockfishSettingsVisible();
    applyTheme();
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* rootLayout = new QVBoxLayout(centralWidget);

    // Top layout (Video + Settings Cog)
    auto* fileLayout = new QHBoxLayout();
    auto* videoLabel = new QLabel("Video File(s):");
    videoLabel->setToolTip("Select the chess video file(s) you want to process");
    fileLayout->addWidget(videoLabel);
    videoPathEdit_ = new QLineEdit();
    videoPathEdit_->setToolTip("Path to the selected video file(s). Separate multiple files with a semicolon (;)");
    fileLayout->addWidget(videoPathEdit_);
    browseBtn_ = new QPushButton("Browse...");
    browseBtn_->setToolTip("Open file dialog to select video file(s)");
    fileLayout->addWidget(browseBtn_);

    settingsBtn_ = new QPushButton();
    settingsBtn_->setObjectName("settingsBtn");
    settingsBtn_->setText("");
    settingsBtn_->setToolTip("Open Settings");
    settingsBtn_->setCursor(Qt::PointingHandCursor);
    settingsBtn_->setFixedSize(32, 32);
    settingsBtn_->setIconSize(QSize(20, 20));
    fileLayout->addWidget(settingsBtn_);

    rootLayout->addLayout(fileLayout);

    // --- Settings Dialog ---
    auto* settingsDialog = new QDialog(this);
    settingsDialog->setWindowTitle("Settings");
    settingsDialog->resize(650, 500);
    auto* dialogLayout = new QVBoxLayout(settingsDialog);

    auto* tabWidget = new QTabWidget();
    dialogLayout->addWidget(tabWidget);

    // === Tab 1: General ===
    auto* generalTab = new QWidget();
    auto* generalLayout = new QVBoxLayout(generalTab);

    // Output Directory Group
    auto* outputDirGroup = new QGroupBox("Output Directory");
    outputDirGroup->setToolTip("Choose where the generated output files will be saved");
    auto* outputDirLayout = new QVBoxLayout(outputDirGroup);

    auto* sameAsSourceRadio = new QRadioButton("Save to same folder as source video");
    sameAsSourceRadio->setObjectName("sameAsSourceRadio");
    sameAsSourceRadio->setChecked(true);
    sameAsSourceRadio->setToolTip("Save output files (PGN, JSON) to the same folder as the input video");
    outputDirLayout->addWidget(sameAsSourceRadio);

    auto* customDirHLayout = new QHBoxLayout();
    auto* customDirRadio = new QRadioButton("Custom directory:");
    customDirRadio->setObjectName("customDirRadio");
    customDirRadio->setToolTip("Save output files to a specific directory");
    customDirHLayout->addWidget(customDirRadio);

    auto* customDirEdit = new QLineEdit();
    customDirEdit->setObjectName("customDirEdit");
    customDirEdit->setEnabled(false);
    customDirEdit->setToolTip("Custom output directory path");
    customDirHLayout->addWidget(customDirEdit);

    auto* customDirBtn = new QPushButton("Browse...");
    customDirBtn->setObjectName("customDirBtn");
    customDirBtn->setEnabled(false);
    customDirBtn->setToolTip("Select a custom output directory");
    customDirHLayout->addWidget(customDirBtn);

    outputDirLayout->addLayout(customDirHLayout);
    generalLayout->addWidget(outputDirGroup);

    connect(sameAsSourceRadio, &QRadioButton::toggled, [customDirEdit, customDirBtn](bool checked) {
        customDirEdit->setEnabled(!checked);
        customDirBtn->setEnabled(!checked);
    });
    connect(sameAsSourceRadio, &QRadioButton::toggled, this, [this]() { saveSettings(); });
    connect(customDirEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });

    // Generation Options
    auto* togglesGroup = new QGroupBox("Generation Options");
    togglesGroup->setToolTip("Configure output options for video processing");
    auto* togglesLayout = new QVBoxLayout(togglesGroup);
    togglesLayout->addWidget(createToggleRow(
        "Generate Game Moves File (PGN)",
        "Exports the extracted moves to a PGN file with clock information and analysis lines looked at by the video creator",
        pgnExportToggle_,
        true
    ));
    togglesLayout->addWidget(createToggleRow(
        "Generate PGN with Stockfish Analysis",
        "Generates a PGN file of the game and includes Stockfish engine evaluations and variations for each move",
        stockfishToggle_,
        false
    ));
    togglesLayout->addWidget(createToggleRow(
        "Generate Analysis Video",
        "Generates an Analysis Video showing the analysis board, Stockfish analysis lines, Engine Score, and an Analysis Bar",
        analysisVideoToggle_,
        false
    ));
    generalLayout->addWidget(togglesGroup);

    // Theme selector
    auto* themeGroup = new QGroupBox("Appearance");
    auto* themeLayout = new QHBoxLayout(themeGroup);
    auto* themeLabel = new QLabel("Theme:");
    themeLabel->setToolTip("Select the application theme (Light, Dark, or follow System settings)");
    themeLayout->addWidget(themeLabel);
    themeComboBox_ = new QComboBox();
    themeComboBox_->addItem("System");
    themeComboBox_->addItem("Light");
    themeComboBox_->addItem("Dark");
    themeComboBox_->setToolTip("Choose between Light, Dark, or System theme");
    themeComboBox_->setProperty("class", "dropdown");
    themeLayout->addWidget(themeComboBox_);
    themeLayout->addStretch();
    generalLayout->addWidget(themeGroup);

    generalLayout->addStretch();
    tabWidget->addTab(generalTab, "General");

    // === Tab 2: Stockfish ===
    auto* stockfishTab = new QWidget();
    auto* stockfishLayout = new QVBoxLayout(stockfishTab);

    stockfishSettingsGroup_ = new QGroupBox("Shared Stockfish Analysis Settings");
    stockfishSettingsGroup_->setToolTip("These settings control the shared Stockfish analysis pass used by PGN export and Analysis Video generation.");
    auto* stockfishOptionsLayout = new QVBoxLayout(stockfishSettingsGroup_);
    stockfishOptionsLayout->setContentsMargins(10, 10, 10, 10);

    auto* stockfishInfoLabel = new QLabel(
        "These settings control the Stockfish analysis run that is reused across the app."
    );
    stockfishInfoLabel->setToolTip("Global settings applied whenever Stockfish evaluation is used.");
    stockfishInfoLabel->setWordWrap(true);
    stockfishOptionsLayout->addWidget(stockfishInfoLabel);

    // Stockfish Executable Path
    auto* stockfishPathLayout = new QHBoxLayout();
    auto* stockfishPathLabel = new QLabel("Stockfish Executable:");
    stockfishPathLabel->setToolTip("Path to your Stockfish executable file (e.g., stockfish.exe)");
    stockfishPathLayout->addWidget(stockfishPathLabel);

    auto* stockfishPathEdit = new QLineEdit();
    stockfishPathEdit->setObjectName("stockfishPathEdit");
    stockfishPathEdit->setToolTip("Leave blank to search for 'stockfish/stockfish.exe' relative to the application");
    stockfishPathLayout->addWidget(stockfishPathEdit);

    auto* stockfishPathBtn = new QPushButton("Browse...");
    stockfishPathBtn->setObjectName("stockfishPathBtn");
    stockfishPathBtn->setToolTip("Browse for the Stockfish executable");
    stockfishPathLayout->addWidget(stockfishPathBtn);

    auto* stockfishSearchBtn = new QPushButton("Auto-Find");
    stockfishSearchBtn->setObjectName("stockfishSearchBtn");
    stockfishSearchBtn->setToolTip("Search common locations to automatically find the Stockfish executable");
    stockfishPathLayout->addWidget(stockfishSearchBtn);
    stockfishOptionsLayout->addLayout(stockfishPathLayout);

    // MultiPV dropdown
    auto* multiPvLayout = new QHBoxLayout();
    auto* multiPvLabel = new QLabel("Lines Per Position:");
    multiPvLabel->setToolTip("How many candidate lines Stockfish should calculate for each position.");
    multiPvLayout->addWidget(multiPvLabel);
    multiPvComboBox_ = new QComboBox();
    multiPvComboBox_->addItems({"1", "2", "3", "4"});
    multiPvComboBox_->setCurrentIndex(2);
    multiPvComboBox_->setToolTip("Choose how many engine lines to generate for each position (1-4).");
    multiPvComboBox_->setProperty("class", "dropdown");
    multiPvLayout->addWidget(multiPvComboBox_);
    multiPvLayout->addStretch();
    stockfishOptionsLayout->addLayout(multiPvLayout);

    // Stockfish Depth
    auto* depthLayout = new QHBoxLayout();
    auto* depthLabel = new QLabel("Engine Search Depth:");
    depthLabel->setToolTip("Maximum search depth for the shared Stockfish analysis run.");
    depthLayout->addWidget(depthLabel);
    auto* depthSpinBox = new QSpinBox();
    depthSpinBox->setObjectName("depthSpinBox");
    depthSpinBox->setRange(1, 40);
    depthSpinBox->setValue(15);
    depthSpinBox->setToolTip("Higher depth usually improves engine analysis quality, but takes longer. Recommended: 15 (Fast) to 20 (Deep).");
    depthLayout->addWidget(depthSpinBox);
    auto* depthHint = new QLabel("(Rec: 15 for older CPUs, 20+ for fast CPUs)");
    depthHint->setToolTip("Suggested search depths based on your hardware capabilities");
    depthLayout->addWidget(depthHint);
    depthLayout->addStretch();
    stockfishOptionsLayout->addLayout(depthLayout);

    // Stockfish Analysis Line Depth
    auto* analysisDepthLayout = new QHBoxLayout();
    auto* analysisDepthLabel = new QLabel("Engine Variation Length:");
    analysisDepthLabel->setToolTip("How many moves from each Stockfish line should be included in the generated outputs.");
    analysisDepthLayout->addWidget(analysisDepthLabel);
    auto* analysisDepthSpinBox = new QSpinBox();
    analysisDepthSpinBox->setObjectName("analysisDepthSpinBox");
    analysisDepthSpinBox->setRange(1, 10);
    analysisDepthSpinBox->setValue(5);
    analysisDepthSpinBox->setToolTip("Choose how many moves from each engine variation to include in the output (1-10).");
    analysisDepthLayout->addWidget(analysisDepthSpinBox);
    auto* analysisDepthHint = new QLabel("(Default: 5 moves)");
    analysisDepthHint->setToolTip("Default number of variation moves is 5");
    analysisDepthLayout->addWidget(analysisDepthHint);
    analysisDepthLayout->addStretch();
    stockfishOptionsLayout->addLayout(analysisDepthLayout);

    stockfishLayout->addWidget(stockfishSettingsGroup_);
    stockfishLayout->addStretch();
    tabWidget->addTab(stockfishTab, "Stockfish");

    // === Tab 3: Advanced ===
    auto* advancedTab = new QWidget();
    auto* advancedLayout = new QVBoxLayout(advancedTab);

    auto* advancedGroup = new QGroupBox("Performance & Debugging");
    auto* advancedGroupLayout = new QVBoxLayout(advancedGroup);

    // Thread count
    auto* threadLayout = new QHBoxLayout();
    auto* threadLabel = new QLabel("FFmpeg Decode Threads:");
    threadLabel->setToolTip("Number of CPU threads for FFmpeg video decoding (1-16). Higher values speed up decoding but use more CPU.");
    threadLayout->addWidget(threadLabel);
    threadSpinBox_ = new QSpinBox();
    threadSpinBox_->setRange(1, 16);
    threadSpinBox_->setValue(4);
    threadSpinBox_->setToolTip("Set the number of threads OpenCV/FFmpeg uses for video decoding");
    threadLayout->addWidget(threadSpinBox_);
    threadLayout->addStretch();
    advancedGroupLayout->addLayout(threadLayout);

    // Debug Level
    auto* debugLevelLayout = new QHBoxLayout();
    auto* debugLevelLabel = new QLabel("Debug Image Generation:");
    debugLevelLabel->setToolTip("Controls how many debug screenshots are saved to disk during extraction.");
    debugLevelLayout->addWidget(debugLevelLabel);
    debugLevelComboBox_ = new QComboBox();
    debugLevelComboBox_->addItems({"None", "Moves Only", "Full"});
    debugLevelComboBox_->setToolTip("None = Fastest, Moves = Save image on each ply, Full = Save all evaluated frames");
    debugLevelComboBox_->setProperty("class", "dropdown");
    debugLevelLayout->addWidget(debugLevelComboBox_);
    debugLevelLayout->addStretch();
    advancedGroupLayout->addLayout(debugLevelLayout);

    advancedLayout->addWidget(advancedGroup);
    advancedLayout->addStretch();
    tabWidget->addTab(advancedTab, "Advanced");

    auto* dialogBtnBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(dialogBtnBox, &QDialogButtonBox::rejected, settingsDialog, &QDialog::accept);
    dialogLayout->addWidget(dialogBtnBox);

    connect(settingsBtn_, &QPushButton::clicked, settingsDialog, &QDialog::exec);
    
    // --- End Settings Dialog ---

    // Main UI Connections
    connect(customDirBtn, &QPushButton::clicked, this, [this, customDirEdit]() {
        QSettings qs(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat);
        QString lastDir = qs.value("lastCustomDir", QDir::homePath()).toString();
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", lastDir);
        if (!dir.isEmpty()) {
            customDirEdit->setText(dir);
            qs.setValue("lastCustomDir", dir);
        }
    });

    connect(debugLevelComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });

    // Log output
    logOutput_ = new QTextEdit();
    logOutput_->setReadOnly(true);
    logOutput_->setToolTip("Processing log output showing progress and messages");
    logOutput_->setMinimumHeight(140);
    rootLayout->addWidget(logOutput_);

    // Progress and Start row
    auto* bottomLayout = new QHBoxLayout();
    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setToolTip("Shows the current progress of video processing");
    bottomLayout->addWidget(progressBar_);

    startCancelBtn_ = new QPushButton("Start Processing");
    startCancelBtn_->setToolTip("Begin processing the selected video file to extract moves and generate analysis");
    bottomLayout->addWidget(startCancelBtn_);
    rootLayout->addLayout(bottomLayout);

    connect(browseBtn_, &QPushButton::clicked, this, &MainWindow::browseVideo);
    connect(startCancelBtn_, &QPushButton::clicked, this, &MainWindow::onStartCancelClicked);
    
    connect(pgnExportToggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
        togglePgnExport(checked);
    });
    connect(stockfishToggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
        toggleStockfish(checked);
    });
    connect(analysisVideoToggle_, &ToggleSwitch::toggled, this, [this](bool checked) { 
        appendLog(checked ? "Analysis Video generation enabled" : "Analysis Video generation disabled");
        saveSettings(); 
    });
    connect(threadSpinBox_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onThreadsChanged);
    connect(multiPvComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { saveSettings(); });
    // Connect new spinboxes to saveSettings
    connect(stockfishPathBtn, &QPushButton::clicked, this, &MainWindow::browseStockfish);
    connect(stockfishSearchBtn, &QPushButton::clicked, this, &MainWindow::autoFindStockfish);
    connect(depthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(analysisDepthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(stockfishPathEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    connect(themeComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onThemeChanged);
}

void MainWindow::setupWorker() {
    worker_ = new VideoProcessorWorker();
    worker_->moveToThread(&workerThread_);

    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &VideoProcessorWorker::logMessage, this, &MainWindow::appendLog);
    connect(worker_, &VideoProcessorWorker::progressUpdated, this, &MainWindow::updateProgress);
    connect(worker_, &VideoProcessorWorker::finished, this, &MainWindow::processingFinished);
    connect(worker_, &VideoProcessorWorker::error, this, &MainWindow::processingError);

    workerThread_.start();
}

void MainWindow::loadSettings() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat);

    // Block signals on all child widgets to prevent partial state saves during loading.
    // This stops auto-save slots from firing and overwriting settings.ini before everything is fully loaded.
    const auto widgets = this->findChildren<QWidget*>();
    for (auto* w : widgets) {
        w->blockSignals(true);
    }

    // Load values from QSettings with defaults and apply to UI
    pgnExportToggle_->setChecked(settings.value("generatePgn", true).toBool());
    stockfishToggle_->setChecked(settings.value("enableStockfish", false).toBool());
    analysisVideoToggle_->setChecked(settings.value("generateAnalysisVideo", false).toBool());

    int multiPv = settings.value("multiPv", 3).toInt();
    int multiPvIdx = multiPvComboBox_->findText(QString::number(multiPv));
    if (multiPvIdx >= 0) {
        multiPvComboBox_->setCurrentIndex(multiPvIdx);
    } else {
        multiPvComboBox_->setCurrentIndex(2); // Default to 3 (index 2)
    }

    threadSpinBox_->setValue(settings.value("ffmpegThreads", 4).toInt());

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    if (depthSpinBox) {
        depthSpinBox->setValue(settings.value("stockfishDepth", 15).toInt());
    }

    auto* analysisDepthSpinBox = findChild<QSpinBox*>("analysisDepthSpinBox");
    if (analysisDepthSpinBox) {
        analysisDepthSpinBox->setValue(settings.value("stockfishAnalysisDepth", 5).toInt());
    }

    auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit");
    if (stockfishPathEdit) {
        stockfishPathEdit->setText(settings.value("stockfishPath", "").toString());
    }
    
    debugLevelComboBox_->setCurrentIndex(settings.value("debugLevel", 0).toInt());

    themeComboBox_->setCurrentIndex(settings.value("themeMode", 0).toInt());

    bool sameAsSource = settings.value("outSameAsSource", true).toBool();
    auto* sameAsSourceRadio = findChild<QRadioButton*>("sameAsSourceRadio");
    auto* customDirRadio = findChild<QRadioButton*>("customDirRadio");
    if (sameAsSourceRadio && customDirRadio) {
        sameAsSource ? sameAsSourceRadio->setChecked(true) : customDirRadio->setChecked(true);
    }

    auto* customDirEdit = findChild<QLineEdit*>("customDirEdit");
    if (customDirEdit) {
        customDirEdit->setText(settings.value("outCustomDir", "").toString());
    }

    // Restore signals
    for (auto* w : widgets) {
        w->blockSignals(false);
    }
}

void MainWindow::saveSettings() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat);

    // Save raw UI state directly, circumventing business logic in gatherSettings()
    // (e.g., gatherSettings forces PGN generation on if Stockfish is on)
    settings.setValue("generatePgn", pgnExportToggle_->isChecked());
    settings.setValue("enableStockfish", stockfishToggle_->isChecked());
    settings.setValue("generateAnalysisVideo", analysisVideoToggle_->isChecked());
    settings.setValue("multiPv", multiPvComboBox_->currentText().toInt());
    settings.setValue("ffmpegThreads", threadSpinBox_->value());
    settings.setValue("themeMode", themeComboBox_->currentIndex());

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    if (depthSpinBox) settings.setValue("stockfishDepth", depthSpinBox->value());

    auto* analysisDepthSpinBox = findChild<QSpinBox*>("analysisDepthSpinBox");
    if (analysisDepthSpinBox) settings.setValue("stockfishAnalysisDepth", analysisDepthSpinBox->value());

    auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit");
    if (stockfishPathEdit) settings.setValue("stockfishPath", stockfishPathEdit->text());

    settings.setValue("debugLevel", debugLevelComboBox_->currentIndex());

    auto* sameAsSourceRadio = findChild<QRadioButton*>("sameAsSourceRadio");
    auto* customDirEdit = findChild<QLineEdit*>("customDirEdit");
    if (sameAsSourceRadio) settings.setValue("outSameAsSource", sameAsSourceRadio->isChecked());
    if (customDirEdit) settings.setValue("outCustomDir", customDirEdit->text());
}

ProcessingSettings MainWindow::gatherSettings() const {
    ProcessingSettings s;
    QString currentVideo = property("currentVideo").toString();
    if (currentVideo.isEmpty()) {
        currentVideo = videoPathEdit_->text().split(";", Qt::SkipEmptyParts).value(0);
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
    auto* sameAsSourceRadio = findChild<QRadioButton*>("sameAsSourceRadio");
    auto* customDirEdit = findChild<QLineEdit*>("customDirEdit");
    
    if (sameAsSourceRadio && sameAsSourceRadio->isChecked()) {
        if (!s.videoPath.isEmpty()) {
            baseDir = QFileInfo(s.videoPath).absolutePath();
        }
    } else if (customDirEdit && !customDirEdit->text().isEmpty()) {
        baseDir = customDirEdit->text();
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
    
    s.generatePgn = pgnExportToggle_->isChecked() || stockfishToggle_->isChecked();
    s.enableStockfish = stockfishToggle_->isChecked();
    s.generateAnalysisVideo = analysisVideoToggle_->isChecked();
    s.multiPv = multiPvComboBox_->currentText().toInt();
    s.ffmpegThreads = threadSpinBox_->value();

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    s.stockfishDepth = depthSpinBox ? depthSpinBox->value() : 15;

    auto* analysisDepthSpinBox = findChild<QSpinBox*>("analysisDepthSpinBox");
    s.stockfishAnalysisDepth = analysisDepthSpinBox ? analysisDepthSpinBox->value() : 5;

    auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit");
    s.stockfishPath = stockfishPathEdit ? stockfishPathEdit->text() : "";
    s.debugLevel = debugLevelComboBox_->currentIndex();
    return s;
}

void MainWindow::applySettingsToUi(const ProcessingSettings& settings) {
    // This function is now primarily for headless mode to apply a settings struct.
    // GUI startup loading is handled by loadSettings().
    pgnExportToggle_->setChecked(settings.generatePgn);
    stockfishToggle_->setChecked(settings.enableStockfish);
    analysisVideoToggle_->setChecked(settings.generateAnalysisVideo);

    int idx = multiPvComboBox_->findText(QString::number(settings.multiPv));
    if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);

    threadSpinBox_->setValue(settings.ffmpegThreads);

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    if (depthSpinBox) depthSpinBox->setValue(settings.stockfishDepth);

    auto* analysisDepthSpinBox = findChild<QSpinBox*>("analysisDepthSpinBox");
    if (analysisDepthSpinBox) analysisDepthSpinBox->setValue(settings.stockfishAnalysisDepth);

    auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit");
    if (stockfishPathEdit) stockfishPathEdit->setText(settings.stockfishPath);
    debugLevelComboBox_->setCurrentIndex(settings.debugLevel);
}

int MainWindow::processHeadless(const QString& videoPath, int pgnOverride, int stockfishOverride, int multiPv, int threads, int depth, int analysisDepth, const QString& debugLevelStr, const QString& outputOverride, const QString& boardAssetOverride) {
    videoPathEdit_->setText(videoPath);
    // Load persisted settings from INI file and apply to UI.
    // This ensures headless mode uses the same settings as the GUI.
    loadSettings();

    // Apply overrides to UI/settings state before gathering
    if (pgnOverride != -1) pgnExportToggle_->setChecked(pgnOverride != 0);
    if (stockfishOverride != -1) stockfishToggle_->setChecked(stockfishOverride != 0);
    if (multiPv > 0) {
        int idx = multiPvComboBox_->findText(QString::number(multiPv));
        if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);
    }
    if (threads > 0) threadSpinBox_->setValue(threads);
    
    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    if (depth > 0 && depthSpinBox) depthSpinBox->setValue(depth);
    
    auto* analysisDepthSpinBox = findChild<QSpinBox*>("analysisDepthSpinBox");
    if (analysisDepth > 0 && analysisDepthSpinBox) analysisDepthSpinBox->setValue(analysisDepth);

    if (!debugLevelStr.isEmpty() && debugLevelComboBox_) {
        if (debugLevelStr == "NONE") debugLevelComboBox_->setCurrentIndex(0);
        else if (debugLevelStr == "MOVES") debugLevelComboBox_->setCurrentIndex(1);
        else if (debugLevelStr == "FULL") debugLevelComboBox_->setCurrentIndex(2);
    }

    // Set FFmpeg threads
    set_ffmpeg_threads(gatherSettings().ffmpegThreads);

    appendLog("=== Headless Mode ===");
    appendLog("Processing: " + videoPath);

    setProperty("headlessOutputOverride", outputOverride);
    setProperty("headlessBoardAssetOverride", boardAssetOverride);
    
    QStringList videos = videoPath.split(";", Qt::SkipEmptyParts);
    if (videos.isEmpty()) return 1;
    
    setProperty("currentVideo", videos.takeFirst().trimmed());
    setProperty("pendingVideos", videos);

    // Build processing settings using the updated properties
    ProcessingSettings settings = gatherSettings();

    // Use QEventLoop to wait for worker to finish
    QEventLoop loop;
    int resultCode = 0;

    QMetaObject::Connection conn1 = connect(worker_, &VideoProcessorWorker::finished, this, [&]() {
        if (property("currentVideo").toString().isEmpty()) {
            appendLog("Headless batch processing finished successfully.");
            resultCode = 0;
            loop.quit();
        }
    });

    QMetaObject::Connection conn2 = connect(worker_, &VideoProcessorWorker::error, this, [&](const QString& msg) {
        if (property("currentVideo").toString().isEmpty()) {
            appendLog("Headless batch processing failed: " + msg);
            resultCode = 1;
            loop.quit();
        }
    });

    // Start processing
    QMetaObject::invokeMethod(worker_, "process", Q_ARG(ProcessingSettings, settings));

    // Wait for completion
    loop.exec();

    // Clean up connections
    disconnect(conn1);
    disconnect(conn2);

    return resultCode;
}

void MainWindow::autoFindStockfish() {
    appendLog("Searching for Stockfish executable in common locations...");
    startCancelBtn_->setEnabled(false); // Prevent starting while searching
    QCoreApplication::processEvents(); // Force UI update

    QString foundPath;
    QString appDir = QCoreApplication::applicationDirPath();
    
    // 1. Check direct relative paths first
#ifdef _WIN32
    QString execName = "stockfish.exe";
#else
    QString execName = "stockfish";
#endif
    QStringList candidatePaths = {
        appDir + "/stockfish/" + execName,
        appDir + "/../stockfish/" + execName,
        appDir + "/../../stockfish/" + execName,
        appDir + "/" + execName
    };
    
    for (const QString& p : candidatePaths) {
        if (QFileInfo::exists(p)) {
            foundPath = QFileInfo(p).absoluteFilePath();
            break;
        }
    }

    // 2. Fast heuristic search in common root directories
    if (foundPath.isEmpty()) {
        QStringList baseDirs = {
            QDir::rootPath(), // e.g., C:\ on Windows
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
            QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        };

        // Safely add Program Files if available
        QString progFiles = qEnvironmentVariable("PROGRAMFILES");
        QString progFilesX86 = qEnvironmentVariable("PROGRAMFILES(X86)");
        if (!progFiles.isEmpty()) baseDirs << progFiles;
        if (!progFilesX86.isEmpty()) baseDirs << progFilesX86;

        for (const QString& base : baseDirs) {
            QDir dir(base);
            if (!dir.exists()) continue;

            // Look for folders at the root level containing "stockfish" (case-insensitive)
            QStringList dirFilters;
            dirFilters << "*stockfish*";
            QFileInfoList subdirs = dir.entryInfoList(dirFilters, QDir::Dirs | QDir::NoDotAndDotDot);

            for (const QFileInfo& subdirInfo : subdirs) {
                // Recursively check inside the matched folder for the executable
                // This is extremely fast since we are only searching inside already-matched "stockfish" folders
#ifdef _WIN32
                QStringList nameFilters = {"*stockfish*.exe"};
#else
                QStringList nameFilters = {"*stockfish*"};
#endif
                QDirIterator it(subdirInfo.absoluteFilePath(), nameFilters, QDir::Files | QDir::Executable, QDirIterator::Subdirectories);
                if (it.hasNext()) {
                    foundPath = it.next();
                    break;
                }
            }
            if (!foundPath.isEmpty()) break;
        }
    }

    if (!foundPath.isEmpty()) {
        auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit");
        if (stockfishPathEdit) {
            stockfishPathEdit->setText(QDir::toNativeSeparators(foundPath));
        }
        appendLog("Found Stockfish at: " + QDir::toNativeSeparators(foundPath));
        saveSettings(); // Save immediately
    } else {
        appendLog("Could not automatically find Stockfish. Please use the Browse button.");
    }
    
    startCancelBtn_->setEnabled(true);
}

void MainWindow::browseStockfish() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat);
    QString lastDir = settings.value("lastStockfishDir", QDir::homePath()).toString();

#ifdef _WIN32
    QString filter = "Executables (*.exe);;All Files (*)";
#else
    QString filter = "All Files (*)";
#endif
    QString fileName = QFileDialog::getOpenFileName(this, "Select Stockfish Executable", lastDir, filter);
    if (!fileName.isEmpty()) {
        auto* stockfishPathEdit = findChild<QLineEdit*>("stockfishPathEdit");
        if (stockfishPathEdit) {
            stockfishPathEdit->setText(fileName);
        }
        settings.setValue("lastStockfishDir", QFileInfo(fileName).absolutePath());
        saveSettings(); // Save immediately after selection
    }
}

void MainWindow::browseVideo() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat);
    QString lastDir = settings.value("lastVideoDir", QDir::homePath()).toString();

    QStringList fileNames = QFileDialog::getOpenFileNames(this, "Select Chess Video(s)", lastDir, "Video Files (*.mp4 *.mkv *.avi);;All Files (*)");
    if (!fileNames.isEmpty()) {
        videoPathEdit_->setText(fileNames.join(";"));
        settings.setValue("lastVideoDir", QFileInfo(fileNames.first()).absolutePath());
    }
}

void MainWindow::onStartCancelClicked() {
    if (isProcessing_) {
        // --- CANCEL ---
        appendLog("Cancellation requested by user...");
        cancelRequested_ = true;
        startCancelBtn_->setEnabled(false); // Disable button until worker confirms cancellation
        startCancelBtn_->setText("Cancelling...");
        setProperty("pendingVideos", QStringList());
    } else {
        // --- START ---
        if (videoPathEdit_->text().isEmpty()) {
            appendLog("Error: Please select a video file.");
            return;
        }

        QStringList videos = videoPathEdit_->text().split(";", Qt::SkipEmptyParts);
        if (videos.isEmpty()) return;
        
        setProperty("currentVideo", videos.takeFirst().trimmed());
        setProperty("pendingVideos", videos);

        auto settings = gatherSettings();
        if (!settings.generatePgn && !settings.enableStockfish && !settings.generateAnalysisVideo) {
            appendLog("Error: No output options selected. Please select at least one of the generation modes.");
            return;
        }

        isProcessing_ = true;
        cancelRequested_ = false; // Reset flag before starting
        startCancelBtn_->setText("Cancel");
        browseBtn_->setEnabled(false);
        progressBar_->setValue(0);
        appendLog("Starting processing...");

        // Set FFmpeg threads before processing
        set_ffmpeg_threads(settings.ffmpegThreads);
        appendLog("FFmpeg decode threads: " + QString::number(settings.ffmpegThreads));

        // Save settings persistently
        saveSettings();

        QMetaObject::invokeMethod(worker_, "process", Q_ARG(ProcessingSettings, settings), Q_ARG(std::atomic<bool>*, &cancelRequested_));
    }
}

void MainWindow::appendLog(const QString& message) { logOutput_->append(message); }
void MainWindow::updateProgress(int percentage) { progressBar_->setValue(percentage); }

void MainWindow::processingFinished() {
    if (cancelRequested_) {
        appendLog("Processing cancelled.");
        setProperty("pendingVideos", QStringList());
        setProperty("currentVideo", QString());
    } else {
        appendLog("Processing finished successfully for: " + property("currentVideo").toString());
        
        QStringList pending = property("pendingVideos").toStringList();
        if (!pending.isEmpty()) {
            QString nextVideo = pending.takeFirst().trimmed();
            setProperty("pendingVideos", pending);
            setProperty("currentVideo", nextVideo);
            
            appendLog("Starting next video: " + nextVideo);
            progressBar_->setValue(0);
            cancelRequested_ = false;
            
            auto settings = gatherSettings();
            QMetaObject::invokeMethod(worker_, "process", Q_ARG(ProcessingSettings, settings), Q_ARG(std::atomic<bool>*, &cancelRequested_));
            return; // Skip UI reset until full batch is done
        }
        
        appendLog("All videos processed successfully.");
        progressBar_->setValue(100);
    }
    isProcessing_ = false;
    startCancelBtn_->setText("Start Processing");
    startCancelBtn_->setEnabled(true);
    browseBtn_->setEnabled(true);
    setProperty("currentVideo", QString());
    QCoreApplication::processEvents();
}

void MainWindow::processingError(const QString& errorMessage) {
    appendLog("Error: " + errorMessage);
    
    QStringList pending = property("pendingVideos").toStringList();
    if (!pending.isEmpty() && !cancelRequested_) {
        QString nextVideo = pending.takeFirst().trimmed();
        setProperty("pendingVideos", pending);
        setProperty("currentVideo", nextVideo);
        
        appendLog("Skipping to next video: " + nextVideo);
        progressBar_->setValue(0);
        
        auto settings = gatherSettings();
        QMetaObject::invokeMethod(worker_, "process", Q_ARG(ProcessingSettings, settings), Q_ARG(std::atomic<bool>*, &cancelRequested_));
        return; // Skip UI reset
    }
    
    isProcessing_ = false;
    startCancelBtn_->setText("Start Processing");
    startCancelBtn_->setEnabled(true);
    browseBtn_->setEnabled(true);
    setProperty("currentVideo", QString());
}

void MainWindow::togglePgnExport(bool checked) {
    appendLog(checked ? "PGN export enabled" : "PGN export disabled");
    saveSettings();
}

void MainWindow::toggleStockfish(bool checked) {
    appendLog(checked ? "PGN with Stockfish analysis enabled" : "PGN with Stockfish analysis disabled");
    saveSettings();
}

void MainWindow::onThreadsChanged(int value) {
    Q_UNUSED(value);
    saveSettings();
}

void MainWindow::onThemeChanged(int index) {
    Q_UNUSED(index);
    applyTheme();
    saveSettings();
    appendLog("Theme changed to: " + themeComboBox_->currentText());
}

void MainWindow::applyTheme() {
    int themeIndex = themeComboBox_->currentIndex();
    ThemeMode mode = static_cast<ThemeMode>(themeIndex);
    ThemeManager::instance().setTheme(mode);
    
    QString styleSheet = ThemeManager::instance().generateStyleSheet();
    qApp->setStyleSheet(styleSheet);
    updateSettingsButtonIcon();
    
    // Force update on all widgets to apply theme
    qApp->processEvents();
}

void MainWindow::updateSettingsButtonIcon() {
    if (!settingsBtn_) {
        return;
    }

    const auto colors = ThemeManager::instance().colors();
    settingsBtn_->setIcon(createSettingsCogIcon(QColor(colors.buttonText)));
}

void MainWindow::ensureStockfishSettingsVisible() {
    // Deprecated: Layout visibility is now fully managed by the Settings QTabWidget.
}

} // namespace aa
