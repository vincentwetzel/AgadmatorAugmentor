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
#include <QCoreApplication>
#include <QSizePolicy>
#include <QSettings>
#include <QEventLoop>
#include <QMetaMethod>
#include <QDir>
#include <QFileInfo>

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

namespace aa {

const char* MainWindow::SETTINGS_ORG = "ChessTubeAnalyzer";
const char* MainWindow::SETTINGS_APP = "ChessTubeAnalyzer";

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("ChessTube Analyzer");
    resize(800, 600);

    setupUi();
    setupWorker();
    loadSettings();
    applySettingsToUi(gatherSettings());
    applyTheme();
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* layout = new QVBoxLayout(centralWidget);

    // File selection row
    auto* fileLayout = new QHBoxLayout();
    auto* videoLabel = new QLabel("Video File:");
    videoLabel->setToolTip("Select the chess video file you want to process");
    fileLayout->addWidget(videoLabel);
    videoPathEdit_ = new QLineEdit();
    videoPathEdit_->setToolTip("Path to the selected video file");
    fileLayout->addWidget(videoPathEdit_);
    browseBtn_ = new QPushButton("Browse...");
    browseBtn_->setToolTip("Open file dialog to select a video file");
    fileLayout->addWidget(browseBtn_);
    layout->addLayout(fileLayout);

    // Settings group box
    auto* settingsGroup = new QGroupBox("Output Settings");
    settingsGroup->setToolTip("Configure output options for video processing");
    auto* settingsLayout = new QVBoxLayout();

    // Output Directory
    auto* outputDirLayout = new QVBoxLayout();
    auto* outputDirLabel = new QLabel("Output Directory:");
    outputDirLayout->addWidget(outputDirLabel);

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
    settingsLayout->addLayout(outputDirLayout);

    connect(sameAsSourceRadio, &QRadioButton::toggled, [customDirEdit, customDirBtn](bool checked) {
        customDirEdit->setEnabled(!checked);
        customDirBtn->setEnabled(!checked);
    });

    connect(sameAsSourceRadio, &QRadioButton::toggled, this, [this]() { saveSettings(); });
    connect(customDirEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });

    // PGN Export toggle
    settingsLayout->addWidget(createToggleRow(
        "Generate PGN file",
        "When enabled, exports the extracted moves to a PGN file with clock information and engine evaluations",
        pgnExportToggle_,
        true
    ));

    // Stockfish Analysis toggle
    settingsLayout->addWidget(createToggleRow(
        "Enable Stockfish engine analysis",
        "When enabled, analyzes each position with Stockfish to provide evaluations and best move suggestions",
        stockfishToggle_,
        false
    ));

    // MultiPV dropdown
    auto* multiPvLayout = new QHBoxLayout();
    auto* multiPvLabel = new QLabel("Best Lines (MultiPV):");
    multiPvLabel->setToolTip("Number of alternative moves/lines Stockfish will suggest for each position");
    multiPvLayout->addWidget(multiPvLabel);
    multiPvComboBox_ = new QComboBox();
    multiPvComboBox_->addItems({"1", "2", "3", "4"});
    multiPvComboBox_->setCurrentIndex(2);
    multiPvComboBox_->setToolTip("Select how many best moves Stockfish should analyze (1-4)");
    multiPvComboBox_->setProperty("class", "dropdown");
    multiPvLayout->addWidget(multiPvComboBox_);
    multiPvLayout->addStretch();
    settingsLayout->addLayout(multiPvLayout);

    // Stockfish Depth
    auto* depthLayout = new QHBoxLayout();
    auto* depthLabel = new QLabel("Analysis Depth:");
    depthLabel->setToolTip("Maximum depth Stockfish will search.");
    depthLayout->addWidget(depthLabel);
    auto* depthSpinBox = new QSpinBox();
    depthSpinBox->setObjectName("depthSpinBox");
    depthSpinBox->setRange(1, 40);
    depthSpinBox->setValue(15);
    depthSpinBox->setToolTip("Higher depth = better analysis but takes longer. Recommended: 15 (Fast) to 20 (Deep).");
    depthLayout->addWidget(depthSpinBox);
    auto* depthHint = new QLabel("(Rec: 15 for older CPUs, 20+ for fast CPUs)");
    depthLayout->addWidget(depthHint);
    depthLayout->addStretch();
    settingsLayout->addLayout(depthLayout);

    // Stockfish Time Limit
    auto* timeLayout = new QHBoxLayout();
    auto* timeLabel = new QLabel("Time per Move (ms):");
    timeLabel->setToolTip("Maximum time Stockfish can spend analyzing a single position.");
    timeLayout->addWidget(timeLabel);
    auto* timeSpinBox = new QSpinBox();
    timeSpinBox->setObjectName("timeSpinBox");
    timeSpinBox->setRange(100, 60000);
    timeSpinBox->setSingleStep(500);
    timeSpinBox->setValue(1000);
    timeSpinBox->setToolTip("Limits the thinking time per move. Prevents the engine from hanging on complex positions.");
    timeLayout->addWidget(timeSpinBox);
    timeLayout->addStretch();
    settingsLayout->addLayout(timeLayout);

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
    settingsLayout->addLayout(threadLayout);

    // Theme selector
    auto* themeLayout = new QHBoxLayout();
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
    settingsLayout->addLayout(themeLayout);

    settingsGroup->setLayout(settingsLayout);
    layout->addWidget(settingsGroup);

    // Connect custom directory browse button
    connect(customDirBtn, &QPushButton::clicked, this, [this, customDirEdit]() {
        QSettings qs(SETTINGS_ORG, SETTINGS_APP);
        QString lastDir = qs.value("lastCustomDir", QDir::homePath()).toString();
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", lastDir);
        if (!dir.isEmpty()) {
            customDirEdit->setText(dir);
            qs.setValue("lastCustomDir", dir);
        }
    });

    // Log output
    logOutput_ = new QTextEdit();
    logOutput_->setReadOnly(true);
    logOutput_->setToolTip("Processing log output showing progress and messages");
    layout->addWidget(logOutput_);

    // Progress and Start row
    auto* bottomLayout = new QHBoxLayout();
    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setToolTip("Shows the current progress of video processing");
    bottomLayout->addWidget(progressBar_);

    startBtn_ = new QPushButton("Start Processing");
    startBtn_->setToolTip("Begin processing the selected video file to extract moves and generate analysis");
    bottomLayout->addWidget(startBtn_);
    layout->addLayout(bottomLayout);

    connect(browseBtn_, &QPushButton::clicked, this, &MainWindow::browseVideo);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::startProcessing);
    connect(pgnExportToggle_, &ToggleSwitch::toggled, this, &MainWindow::togglePgnExport);
    connect(stockfishToggle_, &ToggleSwitch::toggled, this, &MainWindow::toggleStockfish);
    connect(threadSpinBox_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onThreadsChanged);
    // Connect new spinboxes to saveSettings
    connect(depthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
    connect(timeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { saveSettings(); });
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
    // Settings are loaded in applySettingsToUi after UI is built
}

void MainWindow::saveSettings() {
    QSettings settings(SETTINGS_ORG, SETTINGS_APP);

    auto s = gatherSettings();
    settings.setValue("generatePgn", s.generatePgn);
    settings.setValue("enableStockfish", s.enableStockfish);
    settings.setValue("multiPv", s.multiPv);
    settings.setValue("ffmpegThreads", s.ffmpegThreads);
    settings.setValue("themeMode", themeComboBox_->currentIndex());

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    auto* timeSpinBox = findChild<QSpinBox*>("timeSpinBox");
    if (depthSpinBox) settings.setValue("stockfishDepth", depthSpinBox->value());
    if (timeSpinBox) settings.setValue("stockfishTime", timeSpinBox->value());

    auto* sameAsSourceRadio = findChild<QRadioButton*>("sameAsSourceRadio");
    auto* customDirEdit = findChild<QLineEdit*>("customDirEdit");
    if (sameAsSourceRadio) settings.setValue("outSameAsSource", sameAsSourceRadio->isChecked());
    if (customDirEdit) settings.setValue("outCustomDir", customDirEdit->text());
}

ProcessingSettings MainWindow::gatherSettings() const {
    ProcessingSettings s;
    s.videoPath = videoPathEdit_->text();
    
    // Robust path resolution for board asset
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
    
    QString baseName = "analysis";
    if (!s.videoPath.isEmpty()) {
        baseName = QFileInfo(s.videoPath).completeBaseName();
    }
    s.outputPath = QDir(baseDir).filePath(baseName + ".json");
    QDir().mkpath(baseDir); // Ensure output directory exists
    
    s.generatePgn = pgnExportToggle_->isChecked();
    s.enableStockfish = stockfishToggle_->isChecked();
    s.multiPv = multiPvComboBox_->currentText().toInt();
    s.ffmpegThreads = threadSpinBox_->value();

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    auto* timeSpinBox = findChild<QSpinBox*>("timeSpinBox");
    s.stockfishDepth = depthSpinBox ? depthSpinBox->value() : 15;
    s.stockfishTime = timeSpinBox ? timeSpinBox->value() : 1000;
    return s;
}

void MainWindow::applySettingsToUi(const ProcessingSettings& settings) {
    pgnExportToggle_->setChecked(settings.generatePgn);
    stockfishToggle_->setChecked(settings.enableStockfish);

    int idx = multiPvComboBox_->findText(QString::number(settings.multiPv));
    if (idx >= 0) multiPvComboBox_->setCurrentIndex(idx);

    threadSpinBox_->setValue(settings.ffmpegThreads);

    auto* depthSpinBox = findChild<QSpinBox*>("depthSpinBox");
    auto* timeSpinBox = findChild<QSpinBox*>("timeSpinBox");
    if (depthSpinBox) depthSpinBox->setValue(settings.stockfishDepth);
    if (timeSpinBox) timeSpinBox->setValue(settings.stockfishTime);

    // Load theme setting (0=System, 1=Light, 2=Dark)
    QSettings qsettings(SETTINGS_ORG, SETTINGS_APP);
    int themeMode = qsettings.value("themeMode", 0).toInt();
    themeComboBox_->setCurrentIndex(themeMode);

    auto* sameAsSourceRadio = findChild<QRadioButton*>("sameAsSourceRadio");
    auto* customDirRadio = findChild<QRadioButton*>("customDirRadio");
    auto* customDirEdit = findChild<QLineEdit*>("customDirEdit");
    
    bool sameAsSource = qsettings.value("outSameAsSource", true).toBool();
    QString customDir = qsettings.value("outCustomDir", "").toString();
    
    if (sameAsSourceRadio && customDirRadio) {
        if (sameAsSource) sameAsSourceRadio->setChecked(true);
        else customDirRadio->setChecked(true);
    }
    if (customDirEdit) {
        customDirEdit->setText(customDir);
    }
}

int MainWindow::processHeadless(const QString& videoPath) {
    videoPathEdit_->setText(videoPath);
    // Load persisted settings
    QSettings qsettings(SETTINGS_ORG, SETTINGS_APP);
    {
        auto s = gatherSettings();
        s.generatePgn = qsettings.value("generatePgn", true).toBool();
        s.enableStockfish = qsettings.value("enableStockfish", false).toBool();
        s.multiPv = qsettings.value("multiPv", 3).toInt();
        s.ffmpegThreads = qsettings.value("ffmpegThreads", 4).toInt();
    s.stockfishDepth = qsettings.value("stockfishDepth", 15).toInt();
    s.stockfishTime = qsettings.value("stockfishTime", 1000).toInt();
        applySettingsToUi(s);
    }

    // Set FFmpeg threads
    set_ffmpeg_threads(gatherSettings().ffmpegThreads);

    appendLog("=== Headless Mode ===");
    appendLog("Processing: " + videoPath);

    // Build processing settings
    ProcessingSettings settings = gatherSettings();
    settings.videoPath = videoPath;

    // Use QEventLoop to wait for worker to finish
    QEventLoop loop;
    int resultCode = 0;

    QMetaObject::Connection conn1 = connect(worker_, &VideoProcessorWorker::finished, this, [&]() {
        appendLog("Headless processing finished successfully.");
        resultCode = 0;
        loop.quit();
    });

    QMetaObject::Connection conn2 = connect(worker_, &VideoProcessorWorker::error, this, [&](const QString& msg) {
        appendLog("Headless processing failed: " + msg);
        resultCode = 1;
        loop.quit();
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

void MainWindow::browseVideo() {
    QSettings settings(SETTINGS_ORG, SETTINGS_APP);
    QString lastDir = settings.value("lastVideoDir", QDir::homePath()).toString();

    QString fileName = QFileDialog::getOpenFileName(this, "Select Chess Video", lastDir, "Video Files (*.mp4 *.mkv *.avi)");
    if (!fileName.isEmpty()) {
        videoPathEdit_->setText(fileName);
        settings.setValue("lastVideoDir", QFileInfo(fileName).absolutePath());
    }
}

void MainWindow::startProcessing() {
    if (videoPathEdit_->text().isEmpty()) {
        appendLog("Error: Please select a video file.");
        return;
    }

    if (isProcessing_) {
        appendLog("Warning: Processing already in progress.");
        return;
    }

    isProcessing_ = true;
    startBtn_->setEnabled(false);
    browseBtn_->setEnabled(false);
    progressBar_->setValue(0);
    appendLog("Starting processing...");

    auto settings = gatherSettings();

    // Set FFmpeg threads before processing
    set_ffmpeg_threads(settings.ffmpegThreads);
    appendLog("FFmpeg decode threads: " + QString::number(settings.ffmpegThreads));

    // Save settings persistently
    saveSettings();

    QMetaObject::invokeMethod(worker_, "process", Q_ARG(ProcessingSettings, settings));
}

void MainWindow::appendLog(const QString& message) { logOutput_->append(message); }
void MainWindow::updateProgress(int percentage) { progressBar_->setValue(percentage); }

void MainWindow::processingFinished() {
    isProcessing_ = false;
    appendLog("Processing finished successfully.");
    startBtn_->setEnabled(true);
    browseBtn_->setEnabled(true);
    progressBar_->setValue(100);
    QCoreApplication::processEvents();
}

void MainWindow::processingError(const QString& errorMessage) {
    isProcessing_ = false;
    appendLog("Error: " + errorMessage);
    startBtn_->setEnabled(true);
    browseBtn_->setEnabled(true);
}

void MainWindow::togglePgnExport(bool checked) {
    appendLog(checked ? "PGN export enabled" : "PGN export disabled");
    saveSettings();
}

void MainWindow::toggleStockfish(bool checked) {
    appendLog(checked ? "Stockfish analysis enabled" : "Stockfish analysis disabled");
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
    
    // Force update on all widgets to apply theme
    qApp->processEvents();
}

} // namespace aa
