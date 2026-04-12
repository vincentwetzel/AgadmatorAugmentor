#include "MainWindow.h"
#include "VideoProcessorWorker.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QProgressBar>
#include <QSpinBox>

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

namespace aa {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Agadmator Augmentor");
    resize(800, 600);

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* layout = new QVBoxLayout(centralWidget);

    // File selection row
    auto* fileLayout = new QHBoxLayout();
    fileLayout->addWidget(new QLabel("Video File:"));
    videoPathEdit_ = new QLineEdit();
    fileLayout->addWidget(videoPathEdit_);
    browseBtn_ = new QPushButton("Browse...");
    fileLayout->addWidget(browseBtn_);
    layout->addLayout(fileLayout);

    // Log output
    logOutput_ = new QTextEdit();
    logOutput_->setReadOnly(true);
    layout->addWidget(logOutput_);

    // Progress and Start row
    auto* bottomLayout = new QHBoxLayout();
    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    bottomLayout->addWidget(progressBar_);

    startBtn_ = new QPushButton("Start Processing");
    bottomLayout->addWidget(startBtn_);
    layout->addLayout(bottomLayout);

    connect(browseBtn_, &QPushButton::clicked, this, &MainWindow::browseVideo);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::startProcessing);

    // Setup worker and async thread
    worker_ = new VideoProcessorWorker();
    worker_->moveToThread(&workerThread_);

    // Clean up worker safely when the thread stops
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    
    connect(worker_, &VideoProcessorWorker::logMessage, this, &MainWindow::appendLog);
    connect(worker_, &VideoProcessorWorker::progressUpdated, this, &MainWindow::updateProgress);
    connect(worker_, &VideoProcessorWorker::finished, this, &MainWindow::processingFinished);
    connect(worker_, &VideoProcessorWorker::error, this, &MainWindow::processingError);

    workerThread_.start();
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::browseVideo() {
    QString fileName = QFileDialog::getOpenFileName(this, "Select Chess Video", "", "Video Files (*.mp4 *.mkv *.avi)");
    if (!fileName.isEmpty()) {
        videoPathEdit_->setText(fileName);
    }
}

void MainWindow::startProcessing() {
    if (videoPathEdit_->text().isEmpty()) {
        appendLog("Error: Please select a video file.");
        return;
    }

    startBtn_->setEnabled(false);
    browseBtn_->setEnabled(false);
    progressBar_->setValue(0);
    appendLog("Starting processing...");

    QString videoPath = videoPathEdit_->text();
    QString boardAsset = "assets/board/board.png";
    QString outputPath = "output/analysis.json";

    // Invoke worker slots asynchronously
    QMetaObject::invokeMethod(worker_, "process", Q_ARG(QString, videoPath), Q_ARG(QString, boardAsset), Q_ARG(QString, outputPath));
}

void MainWindow::appendLog(const QString& message) { logOutput_->append(message); }
void MainWindow::updateProgress(int percentage) { progressBar_->setValue(percentage); }

void MainWindow::processingFinished() {
    appendLog("Processing finished successfully.");
    startBtn_->setEnabled(true);
    browseBtn_->setEnabled(true);
    progressBar_->setValue(100);
}

void MainWindow::processingError(const QString& errorMessage) {
    appendLog("Error: " + errorMessage);
    startBtn_->setEnabled(true);
    browseBtn_->setEnabled(true);
}

} // namespace aa