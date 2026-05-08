#include "MainWindow.h"
#include "VideoProcessorWorker.h"
#include "SettingsDialog.h"
#include "TemplateManager.h"

#include <QListWidget>
#include <QListWidgetItem>
#include <QComboBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QPushButton>
#include <QStringList>

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

namespace cta {

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

void MainWindow::startProcessingItem(QListWidgetItem* item) {
    if (!item) {
        finishProcessingSession();
        return;
    }

    const QString path = item->data(PathRole).toString();
    setProperty("currentVideo", path);
    setItemStatus(item, QueueItemStatus::Processing);
    queueList_->setCurrentItem(item);
    queueList_->scrollToItem(item);
    setItemProgress(item, 0);

    if (auto* itemWidget = queueList_->itemWidget(item)) {
        if (auto* tplCombo = itemWidget->findChild<QComboBox*>("queueTemplateCombo")) {
            const QString visibleTemplateId = tplCombo->currentData().toString();
            if (!visibleTemplateId.isEmpty()) {
                applyTemplateToItem(item, visibleTemplateId);
            }
        }
    }

    auto settings = gatherSettings();
    settings.overlayConfig = overlayConfigForItem(item);
    appendLog("Using template \"" + templateNameForItem(item) + "\" for: " + QFileInfo(path).fileName());

    item->setData(OutputDirRole, QFileInfo(settings.outputPath).absolutePath());
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, settings, cancelFlag = &cancelRequested_]() {
            worker->process(settings, cancelFlag);
        },
        Qt::QueuedConnection
    );
}

void MainWindow::startNextQueuedItem() {
    auto* item = nextQueuedItem();
    if (!item) {
        appendLog("No queued videos remain.");
        finishProcessingSession();
        return;
    }

    const QString path = item->data(PathRole).toString();
    appendLog("Starting video: " + path);
    startProcessingItem(item);
}

void MainWindow::finishProcessingSession() {
    isProcessing_ = false;
    cancelRequested_ = false;
    startCancelBtn_->setText("Start Processing");
    startCancelBtn_->setEnabled(true);
    setProperty("currentVideo", QString());
    refreshQueueUi();
    QCoreApplication::processEvents();
}

int MainWindow::processHeadless(const QString& videoPath, int pgnOverride, int stockfishOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, const QString& outputOverride, const QString& boardAssetOverride, int memoryLimit) {
    logTimer_.restart();
    addVideosToQueue(videoPath.split(";", Qt::SkipEmptyParts));
    
    settingsDialog_->loadSettings();
    settingsDialog_->applyHeadlessOverrides(pgnOverride, stockfishOverride, multiPv, threads, depth, time, nodes, analysisDepth, debugLevelStr, memoryLimit);

    set_ffmpeg_threads(gatherSettings().ffmpegThreads);

    appendLog("=== Headless Mode ===");
    appendLog("Processing: " + videoPath);

    setProperty("headlessOutputOverride", outputOverride);
    setProperty("headlessBoardAssetOverride", boardAssetOverride);
    if (!hasQueuedItems()) return 1;

    QEventLoop loop;
    int resultCode = 0;
    auto computeHeadlessResult = [this]() {
        for (int i = 0; i < queueList_->count(); ++i) {
            if (itemStatus(queueList_->item(i)) == QueueItemStatus::Failed) return 1;
        }
        return 0;
    };

    QMetaObject::Connection conn1 = connect(worker_, &VideoProcessorWorker::finished, this, [&]() {
        if (property("currentVideo").toString().isEmpty()) {
            appendLog("Headless batch processing finished.");
            resultCode = computeHeadlessResult();
            loop.quit();
        }
    });

    QMetaObject::Connection conn2 = connect(worker_, &VideoProcessorWorker::error, this, [&](const QString& msg) {
        Q_UNUSED(msg);
        if (property("currentVideo").toString().isEmpty()) {
            resultCode = computeHeadlessResult();
            loop.quit();
        }
    });

    isProcessing_ = true;
    cancelRequested_ = false;
    refreshQueueUi();
    startNextQueuedItem();

    loop.exec();
    disconnect(conn1);
    disconnect(conn2);
    return resultCode;
}

void MainWindow::onStartCancelClicked() {
    if (isProcessing_) {
        appendLog("Cancellation requested for the current video...");
        cancelRequested_ = true;
        startCancelBtn_->setEnabled(false);
        startCancelBtn_->setText("Cancelling...");
    } else {
        if (!hasQueuedItems()) { appendLog("Error: Please add at least one queued video to process."); return; }
        auto settings = gatherSettings();
        if (!settings.generatePgn && !settings.enableStockfish && !settings.generateAnalysisVideo) { appendLog("Error: No output options selected. Please select at least one of the generation modes."); return; }

        isProcessing_ = true;
        cancelRequested_ = false;
        logTimer_.restart();
        appendLog("Starting processing...");

        set_ffmpeg_threads(settings.ffmpegThreads);
        appendLog("FFmpeg decode threads: " + QString::number(settings.ffmpegThreads));

        settingsDialog_->saveSettings();
        refreshQueueUi();
        startNextQueuedItem();
    }
}

void MainWindow::updateProgress(int percentage) {
    auto* currentItem = findQueueItemByPath(property("currentVideo").toString());
    setItemProgress(currentItem, percentage);
}

void MainWindow::processingFinished() {
    const QString finishedVideo = property("currentVideo").toString();
    auto* finishedItem = findQueueItemByPath(finishedVideo);

    if (cancelRequested_) {
        appendLog("Processing cancelled.");
        setItemStatus(finishedItem, QueueItemStatus::Cancelled);
        finishProcessingSession();
        return;
    } else {
        appendLog("Processing finished successfully for: " + finishedVideo);
        setItemProgress(finishedItem, 100);
        setItemStatus(finishedItem, QueueItemStatus::Completed);
    }

    if (hasQueuedItems()) {
        startNextQueuedItem();
        return;
    }
    appendLog("All queued videos finished.");
    finishProcessingSession();
}

void MainWindow::processingError(const QString& errorMessage) {
    appendLog("Error: " + errorMessage);
    auto* currentItem = findQueueItemByPath(property("currentVideo").toString());

    if (cancelRequested_) {
        setItemStatus(currentItem, QueueItemStatus::Cancelled);
        finishProcessingSession();
        return;
    }
    setItemStatus(currentItem, QueueItemStatus::Failed);

    if (hasQueuedItems()) {
        appendLog("Continuing to the next queued video...");
        startNextQueuedItem();
        return;
    }
    finishProcessingSession();
}

} // namespace cta