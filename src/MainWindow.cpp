// Extracted from cpp directory
#include "MainWindow.h"
#include "VideoProcessorWorker.h"
#include "ToggleSwitch.h"
#include "ThemeManager.h"
#include "SettingsDialog.h"
#include "TemplateManager.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QRadioButton>
#include <QListWidget>
#include <QListWidgetItem>
#include <QFrame>
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
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QVariantMap>
#include <QDebug>
#include <QtMath>
#include <algorithm>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

constexpr int kQueuePathRole = Qt::UserRole;
constexpr int kQueueStatusRole = Qt::UserRole + 1;
constexpr int kQueueProgressRole = Qt::UserRole + 2;
constexpr int kQueueOutputDirRole = Qt::UserRole + 3;
constexpr int kQueueTemplateRole = Qt::UserRole + 4;
constexpr int kQueueTemplateNameRole = Qt::UserRole + 5;
constexpr int kQueueTemplateConfigRole = Qt::UserRole + 6;

QVariantMap overlayConfigToVariantMap(const cta::VideoOverlayConfig& config) {
    QVariantMap map;
    map.insert("boardEnabled", config.board.enabled);
    map.insert("boardX", config.board.x_percent);
    map.insert("boardY", config.board.y_percent);
    map.insert("boardScale", config.board.scale);
    map.insert("evalEnabled", config.evalBar.enabled);
    map.insert("evalX", config.evalBar.x_percent);
    map.insert("evalY", config.evalBar.y_percent);
    map.insert("evalScale", config.evalBar.scale);
    map.insert("pvEnabled", config.pvText.enabled);
    map.insert("pvX", config.pvText.x_percent);
    map.insert("pvY", config.pvText.y_percent);
    map.insert("pvScale", config.pvText.scale);
    map.insert("arrowsTarget", QString::fromStdString(config.arrowsTarget));
    return map;
}

cta::VideoOverlayConfig overlayConfigFromVariantMap(const QVariant& value) {
    cta::VideoOverlayConfig config;
    const QVariantMap map = value.toMap();
    if (map.isEmpty()) {
        return config;
    }

    config.board.enabled = map.value("boardEnabled", config.board.enabled).toBool();
    config.board.x_percent = map.value("boardX", config.board.x_percent).toDouble();
    config.board.y_percent = map.value("boardY", config.board.y_percent).toDouble();
    config.board.scale = map.value("boardScale", config.board.scale).toDouble();

    config.evalBar.enabled = map.value("evalEnabled", config.evalBar.enabled).toBool();
    config.evalBar.x_percent = map.value("evalX", config.evalBar.x_percent).toDouble();
    config.evalBar.y_percent = map.value("evalY", config.evalBar.y_percent).toDouble();
    config.evalBar.scale = map.value("evalScale", config.evalBar.scale).toDouble();

    config.pvText.enabled = map.value("pvEnabled", config.pvText.enabled).toBool();
    config.pvText.x_percent = map.value("pvX", config.pvText.x_percent).toDouble();
    config.pvText.y_percent = map.value("pvY", config.pvText.y_percent).toDouble();
    config.pvText.scale = map.value("pvScale", config.pvText.scale).toDouble();

    config.arrowsTarget = map.value("arrowsTarget", QString::fromStdString(config.arrowsTarget)).toString().toStdString();
    return config;
}

QString queueStatusText(cta::MainWindow::QueueItemStatus status) {
    switch (status) {
    case cta::MainWindow::QueueItemStatus::Queued:
        return "Queued";
    case cta::MainWindow::QueueItemStatus::Processing:
        return "Processing";
    case cta::MainWindow::QueueItemStatus::Completed:
        return "Completed";
    case cta::MainWindow::QueueItemStatus::Failed:
        return "Failed";
    case cta::MainWindow::QueueItemStatus::Cancelled:
        return "Cancelled";
    }

    return "Queued";
}

} // namespace

static void set_ffmpeg_threads(int threads) {
    std::string val = std::to_string(threads);
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_THREADS", val.c_str());
#else
    setenv("OPENCV_FFMPEG_THREADS", val.c_str(), 1);
#endif
}

namespace cta {

const char* MainWindow::SETTINGS_ORG = "ChessTubeAnalyzer";
const char* MainWindow::SETTINGS_APP = "ChessTubeAnalyzer";

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("ChessTube Analyzer");
    resize(800, 600);

    setupUi();
    setupWorker();
    settingsDialog_->loadSettings(); // Load all settings from INI file
    applyTheme();
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
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

MainWindow::QueueItemStatus MainWindow::itemStatus(const QListWidgetItem* item) const {
    if (!item) {
        return QueueItemStatus::Queued;
    }

    return static_cast<QueueItemStatus>(item->data(kQueueStatusRole).toInt());
}

void MainWindow::setItemStatus(QListWidgetItem* item, QueueItemStatus status) {
    if (!item) {
        return;
    }

    item->setData(kQueueStatusRole, static_cast<int>(status));
    refreshQueueItem(item);
}

void MainWindow::setItemProgress(QListWidgetItem* item, int percentage) {
    if (!item) {
        return;
    }

    item->setData(kQueueProgressRole, qBound(0, percentage, 100));
    refreshQueueItem(item);
}

void MainWindow::applyTemplateToItem(QListWidgetItem* item, const QString& templateId) const {
    if (!item) {
        return;
    }

    auto optTpl = cta::TemplateManager::instance().getTemplate(templateId);
    const auto tpl = optTpl.has_value() ? optTpl.value() : cta::TemplateManager::instance().getFallbackTemplate();

    item->setData(kQueueTemplateRole, tpl.id);
    item->setData(kQueueTemplateNameRole, tpl.name);
    item->setData(kQueueTemplateConfigRole, overlayConfigToVariantMap(tpl.config));
}

VideoOverlayConfig MainWindow::overlayConfigForItem(const QListWidgetItem* item) const {
    if (!item) {
        return cta::TemplateManager::instance().getFallbackTemplate().config;
    }

    const QVariant storedConfig = item->data(kQueueTemplateConfigRole);
    if (storedConfig.isValid()) {
        return overlayConfigFromVariantMap(storedConfig);
    }

    const QString templateId = item->data(kQueueTemplateRole).toString();
    auto optTpl = cta::TemplateManager::instance().getTemplate(templateId);
    if (optTpl.has_value()) {
        return optTpl->config;
    }

    return cta::TemplateManager::instance().getFallbackTemplate().config;
}

QString MainWindow::templateNameForItem(const QListWidgetItem* item) const {
    if (!item) {
        return cta::TemplateManager::instance().getFallbackTemplate().name;
    }

    const QString storedName = item->data(kQueueTemplateNameRole).toString();
    if (!storedName.isEmpty()) {
        return storedName;
    }

    const QString templateId = item->data(kQueueTemplateRole).toString();
    auto optTpl = cta::TemplateManager::instance().getTemplate(templateId);
    if (optTpl.has_value()) {
        return optTpl->name;
    }

    return cta::TemplateManager::instance().getFallbackTemplate().name;
}

QWidget* MainWindow::createQueueItemWidget(QListWidgetItem* item) const {
    const QString path = item->data(kQueuePathRole).toString();
    const QString fileName = QFileInfo(path).fileName();
    const QueueItemStatus status = itemStatus(item);
    const int progress = item->data(kQueueProgressRole).toInt();
    const QString outputDir = item->data(kQueueOutputDirRole).toString();
    const QString templateId = item->data(kQueueTemplateRole).toString();

    auto* container = new QFrame();
    container->setObjectName("queueItemContainer");
    container->setToolTip(path + "\nStatus: " + queueStatusText(status));
    container->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    auto* nameLabel = new QLabel(fileName);
    nameLabel->setObjectName("nameLabel");
    nameLabel->setToolTip(path);
    QFont nameFont = nameLabel->font();
    nameFont.setBold(status == QueueItemStatus::Processing);
    nameLabel->setFont(nameFont);
    topRow->addWidget(nameLabel, 1);

    auto* statusLabel = new QLabel(queueStatusText(status));
    statusLabel->setObjectName("statusLabel");
    statusLabel->setToolTip("Current processing status for this queued video.");
    topRow->addWidget(statusLabel, 0, Qt::AlignRight);
    layout->addLayout(topRow);

    auto* pathLabel = new QLabel(path);
    pathLabel->setObjectName("pathLabel");
    pathLabel->setToolTip(path);
    pathLabel->setWordWrap(true);
    layout->addWidget(pathLabel);

    auto* templateRow = new QHBoxLayout();
    templateRow->setContentsMargins(0, 0, 0, 0);
    templateRow->setSpacing(8);
    
    auto* tplLabel = new QLabel("Template:");
    tplLabel->setToolTip("The overlay template used to position elements in the analysis video.");
    templateRow->addWidget(tplLabel);
    
    auto* tplCombo = new QComboBox();
    tplCombo->setObjectName("queueTemplateCombo");
    tplCombo->setToolTip("Select the analysis overlay layout tailored for this video.");
    const auto templates = cta::TemplateManager::instance().getAllTemplates();
    for (const auto& t : templates) {
        tplCombo->addItem(t.name, t.id);
    }
    
    int idx = -1;
    for (int i = 0; i < tplCombo->count(); ++i) {
        if (tplCombo->itemData(i).toString() == templateId) {
            idx = i;
            break;
        }
    }
    
    if (idx >= 0) {
        applyTemplateToItem(item, templateId);
        tplCombo->setCurrentIndex(idx);
    } else if (tplCombo->count() > 0) {
        tplCombo->setCurrentIndex(0);
        applyTemplateToItem(item, tplCombo->itemData(0).toString());
    }
    tplCombo->setEnabled(status == QueueItemStatus::Queued);
    connect(tplCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), container, [item, tplCombo, this](int index) {
        if (index >= 0) {
            QString newId = tplCombo->itemData(index).toString();
            applyTemplateToItem(item, newId);
            lastUsedTemplateId_ = newId; // Update memory for subsequent drops
        }
    });
    templateRow->addWidget(tplCombo, 1);
    layout->addLayout(templateRow);

    auto* progressBar = new QProgressBar();
    progressBar->setObjectName("progressBar");
    progressBar->setRange(0, 100);
    progressBar->setValue(progress);
    progressBar->setFormat(QString::number(progress) + "%");
    progressBar->setToolTip("Progress for this video. Active while processing and preserved after completion.");
    progressBar->setEnabled(status != QueueItemStatus::Queued);
    
    auto* progressRow = new QHBoxLayout();
    progressRow->setContentsMargins(0, 0, 0, 0);
    progressRow->setSpacing(8);
    progressRow->addWidget(progressBar, 1);

    auto* openFolderBtn = new QPushButton("Open Folder");
    openFolderBtn->setObjectName("openFolderBtn");
    openFolderBtn->setToolTip("Open the output folder for this processed video.");
    openFolderBtn->setVisible(status == QueueItemStatus::Completed && !outputDir.isEmpty());
    openFolderBtn->setEnabled(status == QueueItemStatus::Completed && !outputDir.isEmpty());
    QObject::connect(openFolderBtn, &QPushButton::clicked, container, [item]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(kQueueOutputDirRole).toString()));
    });
    progressRow->addWidget(openFolderBtn);

    layout->addLayout(progressRow);

    return container;
}

void MainWindow::refreshQueueItem(QListWidgetItem* item) {
    if (!item) {
        return;
    }

    const QString path = item->data(kQueuePathRole).toString();
    const QueueItemStatus status = itemStatus(item);
    int progress = item->data(kQueueProgressRole).toInt();

    if (status == QueueItemStatus::Completed) {
        progress = 100;
        item->setData(kQueueProgressRole, progress);
    } else if ((status == QueueItemStatus::Failed || status == QueueItemStatus::Cancelled) && progress == 100) {
        progress = 0;
        item->setData(kQueueProgressRole, progress);
    }

    item->setToolTip(path + "\nStatus: " + queueStatusText(status));
    // Increase height to accommodate the new Template dropdown row
    item->setSizeHint(QSize(0, 128));

    if (auto* existingWidget = queueList_->itemWidget(item)) {
        existingWidget->setToolTip(path + "\nStatus: " + queueStatusText(status));

        if (auto* statusLabel = existingWidget->findChild<QLabel*>("statusLabel")) {
            statusLabel->setText(queueStatusText(status));
        }
        if (auto* nameLabel = existingWidget->findChild<QLabel*>("nameLabel")) {
            QFont nameFont = nameLabel->font();
            nameFont.setBold(status == QueueItemStatus::Processing);
            nameLabel->setFont(nameFont);
        }
        if (auto* progressBar = existingWidget->findChild<QProgressBar*>("progressBar")) {
            progressBar->setValue(progress);
            progressBar->setFormat(QString::number(progress) + "%");
            progressBar->setEnabled(status != QueueItemStatus::Queued);
        }
        if (auto* openFolderBtn = existingWidget->findChild<QPushButton*>("openFolderBtn")) {
            const QString outputDir = item->data(kQueueOutputDirRole).toString();
            openFolderBtn->setVisible(status == QueueItemStatus::Completed && !outputDir.isEmpty());
            openFolderBtn->setEnabled(status == QueueItemStatus::Completed && !outputDir.isEmpty());
        }
        if (auto* tplCombo = existingWidget->findChild<QComboBox*>("queueTemplateCombo")) {
            const QString templateId = item->data(kQueueTemplateRole).toString();
            tplCombo->blockSignals(true);
            tplCombo->clear();
            const auto templates = cta::TemplateManager::instance().getAllTemplates();
            int idx = 0;
            for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
                tplCombo->addItem(templates[i].name, templates[i].id);
                if (templates[i].id == templateId) {
                    idx = i;
                }
            }
            if (tplCombo->count() > 0) {
                applyTemplateToItem(item, tplCombo->itemData(idx).toString());
                tplCombo->setCurrentIndex(idx);
            }
            tplCombo->setEnabled(status == QueueItemStatus::Queued);
            tplCombo->blockSignals(false);
        }
    } else {
        queueList_->setItemWidget(item, createQueueItemWidget(item));
    }
}

QListWidgetItem* MainWindow::findQueueItemByPath(const QString& path) const {
    for (int i = 0; i < queueList_->count(); ++i) {
        auto* item = queueList_->item(i);
        if (item->data(kQueuePathRole).toString().compare(path, Qt::CaseInsensitive) == 0) {
            return item;
        }
    }

    return nullptr;
}

QListWidgetItem* MainWindow::nextQueuedItem() const {
    for (int i = 0; i < queueList_->count(); ++i) {
        auto* item = queueList_->item(i);
        if (itemStatus(item) == QueueItemStatus::Queued) {
            return item;
        }
    }

    return nullptr;
}

bool MainWindow::hasQueuedItems() const {
    return nextQueuedItem() != nullptr;
}

bool MainWindow::hasRemovableItems() const {
    for (int i = 0; i < queueList_->count(); ++i) {
        auto* item = queueList_->item(i);
        if (itemStatus(item) != QueueItemStatus::Processing) {
            return true;
        }
    }

    return false;
}

bool MainWindow::canMoveSelectionUp() const {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) {
        return false;
    }

    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) {
            return false;
        }

        const int row = queueList_->row(item);
        if (row > 0 && itemStatus(queueList_->item(row - 1)) != QueueItemStatus::Processing) {
            return true;
        }
    }

    return false;
}

bool MainWindow::canMoveSelectionDown() const {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) {
        return false;
    }

    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) {
            return false;
        }

        const int row = queueList_->row(item);
        if (row >= 0 && row < queueList_->count() - 1 && itemStatus(queueList_->item(row + 1)) != QueueItemStatus::Processing) {
            return true;
        }
    }

    return false;
}

void MainWindow::startProcessingItem(QListWidgetItem* item) {
    if (!item) {
        finishProcessingSession();
        return;
    }

    const QString path = item->data(kQueuePathRole).toString();
    setProperty("currentVideo", path);
    setItemStatus(item, QueueItemStatus::Processing);
    queueList_->setCurrentItem(item);
    queueList_->scrollToItem(item);
    setItemProgress(item, 0);

    // Treat the visible queue dropdown as the final source of truth right before launch.
    if (auto* itemWidget = queueList_->itemWidget(item)) {
        if (auto* tplCombo = itemWidget->findChild<QComboBox*>("queueTemplateCombo")) {
            const QString visibleTemplateId = tplCombo->currentData().toString();
            if (!visibleTemplateId.isEmpty()) {
                applyTemplateToItem(item, visibleTemplateId);
            }
        }
    }

    auto settings = gatherSettings();
    
    // Inject the layout configuration from the selected template
    settings.overlayConfig = overlayConfigForItem(item);
    appendLog("Using template \"" + templateNameForItem(item) + "\" for: " + QFileInfo(path).fileName());

    item->setData(kQueueOutputDirRole, QFileInfo(settings.outputPath).absolutePath());
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

    const QString path = item->data(kQueuePathRole).toString();
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

void MainWindow::refreshQueueUi() {
    const int count = queueList_ ? queueList_->count() : 0;
    const bool hasItems = count > 0;

    if (queueEmptyStateLabel_) {
        queueEmptyStateLabel_->setVisible(!hasItems);
    }

    removeSelectedBtn_->setEnabled(hasItems && hasRemovableItems());
    clearQueueBtn_->setEnabled(hasItems && hasRemovableItems());
    moveUpBtn_->setEnabled(hasItems && canMoveSelectionUp());
    moveDownBtn_->setEnabled(hasItems && canMoveSelectionDown());
    browseBtn_->setEnabled(true);

    if (isProcessing_) {
        startCancelBtn_->setText("Cancel Current");
        startCancelBtn_->setEnabled(true);
        queueHelperLabel_->setText("Queue is live: add more videos while processing. Select non-processing items and press Delete to remove them.");
    } else {
        startCancelBtn_->setText("Start Processing");
        startCancelBtn_->setEnabled(hasQueuedItems());
        queueHelperLabel_->setText("Drag and drop video files into the queue below, or click Add Video(s)...");
    }
}

void MainWindow::addVideosToQueue(const QStringList& paths) {
    QStringList addedNames;

    for (const QString& rawPath : paths) {
        const QString localPath = QFileInfo(rawPath).absoluteFilePath();
        QFileInfo info(localPath);
        if (!info.exists() || !info.isFile()) {
            continue;
        }

        const QString suffix = info.suffix().toLower();
        if (suffix != "mp4" && suffix != "mkv" && suffix != "avi" && suffix != "mov" && suffix != "webm") {
            continue;
        }

        bool alreadyQueued = false;
        alreadyQueued = findQueueItemByPath(localPath) != nullptr;
        if (alreadyQueued) {
            continue;
        }

        auto* item = new QListWidgetItem();
        item->setData(kQueuePathRole, localPath);
        item->setData(kQueueStatusRole, static_cast<int>(QueueItemStatus::Queued));
        item->setData(kQueueProgressRole, 0);
        
        auto matchedTpl = cta::TemplateManager::instance().matchTemplate(info.fileName());
        applyTemplateToItem(item, matchedTpl.id);

        queueList_->addItem(item);
        refreshQueueItem(item);
        appendLog("Queued \"" + info.fileName() + "\" with template \"" + templateNameForItem(item) + "\".");
        addedNames << info.fileName();
    }

    if (!addedNames.isEmpty()) {
        queueList_->scrollToBottom();
        appendLog("Queued " + QString::number(addedNames.size()) + " video(s).");
        if (isProcessing_) {
            appendLog("New videos will start automatically after the current batch item finishes.");
        }
    }

    refreshQueueUi();
}

int MainWindow::processHeadless(const QString& videoPath, int pgnOverride, int stockfishOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, const QString& outputOverride, const QString& boardAssetOverride, int memoryLimit) {
    addVideosToQueue(videoPath.split(";", Qt::SkipEmptyParts));
    
    settingsDialog_->loadSettings();
    settingsDialog_->applyHeadlessOverrides(pgnOverride, stockfishOverride, multiPv, threads, depth, time, nodes, analysisDepth, debugLevelStr, memoryLimit);

    // Set FFmpeg threads
    set_ffmpeg_threads(gatherSettings().ffmpegThreads);

    appendLog("=== Headless Mode ===");
    appendLog("Processing: " + videoPath);

    setProperty("headlessOutputOverride", outputOverride);
    setProperty("headlessBoardAssetOverride", boardAssetOverride);
    if (!hasQueuedItems()) return 1;

    // Use QEventLoop to wait for worker to finish
    QEventLoop loop;
    int resultCode = 0;
    auto computeHeadlessResult = [this]() {
        for (int i = 0; i < queueList_->count(); ++i) {
            if (itemStatus(queueList_->item(i)) == QueueItemStatus::Failed) {
                return 1;
            }
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

    // Wait for completion
    loop.exec();

    // Clean up connections
    disconnect(conn1);
    disconnect(conn2);

    return resultCode;
}

void MainWindow::browseVideo() {
    QSettings settings;
    QString lastDir = settings.value("lastVideoDir", QDir::homePath()).toString();

    QStringList fileNames = QFileDialog::getOpenFileNames(this, "Select Chess Video(s)", lastDir, "Video Files (*.mp4 *.mkv *.avi);;All Files (*)");
    if (!fileNames.isEmpty()) {
        addVideosToQueue(fileNames);
        settings.setValue("lastVideoDir", QFileInfo(fileNames.first()).absolutePath());
    }
}

void MainWindow::moveSelectedVideosUp() {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) {
        return;
    }

    QList<int> rows;
    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) {
            continue;
        }
        rows.append(queueList_->row(item));
    }

    std::sort(rows.begin(), rows.end());

    bool moved = false;
    for (int row : rows) {
        if (row <= 0) {
            continue;
        }

        auto* aboveItem = queueList_->item(row - 1);
        if (!aboveItem || aboveItem->isSelected() || itemStatus(aboveItem) == QueueItemStatus::Processing) {
            continue;
        }

        auto* item = queueList_->takeItem(row);
        queueList_->insertItem(row - 1, item);
        item->setSelected(true);
        moved = true;
    }

    if (moved) {
        appendLog("Moved selected queue item(s) up.");
    }
    refreshQueueUi();
}

void MainWindow::moveSelectedVideosDown() {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) {
        return;
    }

    QList<int> rows;
    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) {
            continue;
        }
        rows.append(queueList_->row(item));
    }

    std::sort(rows.begin(), rows.end(), std::greater<int>());

    bool moved = false;
    for (int row : rows) {
        if (row < 0 || row >= queueList_->count() - 1) {
            continue;
        }

        auto* belowItem = queueList_->item(row + 1);
        if (!belowItem || belowItem->isSelected() || itemStatus(belowItem) == QueueItemStatus::Processing) {
            continue;
        }

        auto* item = queueList_->takeItem(row);
        queueList_->insertItem(row + 1, item);
        item->setSelected(true);
        moved = true;
    }

    if (moved) {
        appendLog("Moved selected queue item(s) down.");
    }
    refreshQueueUi();
}

void MainWindow::removeSelectedVideos() {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) {
        return;
    }

    int removedCount = 0;
    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) {
            continue;
        }
        delete queueList_->takeItem(queueList_->row(item));
        ++removedCount;
    }

    if (removedCount > 0) {
        appendLog("Removed " + QString::number(removedCount) + " queue item(s).");
    }
    refreshQueueUi();
}

void MainWindow::clearQueue() {
    if (queueList_->count() == 0) {
        return;
    }

    for (int i = queueList_->count() - 1; i >= 0; --i) {
        auto* item = queueList_->item(i);
        if (itemStatus(item) == QueueItemStatus::Processing) {
            continue;
        }
        delete queueList_->takeItem(i);
    }

    appendLog(isProcessing_ ? "Cleared all non-processing queue items." : "Cleared the video queue.");
    refreshQueueUi();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
        if (suffix == "mp4" || suffix == "mkv" || suffix == "avi" || suffix == "mov" || suffix == "webm") {
            event->acceptProposedAction();
            return;
        }
    }

    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList droppedFiles;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            droppedFiles << url.toLocalFile();
        }
    }

    addVideosToQueue(droppedFiles);
    if (!droppedFiles.isEmpty()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::onStartCancelClicked() {
    if (isProcessing_) {
        // --- CANCEL ---
        appendLog("Cancellation requested for the current video...");
        cancelRequested_ = true;
        startCancelBtn_->setEnabled(false); // Disable button until worker confirms cancellation
        startCancelBtn_->setText("Cancelling...");
    } else {
        // --- START ---
        if (!hasQueuedItems()) {
            appendLog("Error: Please add at least one queued video to process.");
            return;
        }

        auto settings = gatherSettings();
        if (!settings.generatePgn && !settings.enableStockfish && !settings.generateAnalysisVideo) {
            appendLog("Error: No output options selected. Please select at least one of the generation modes.");
            return;
        }

        isProcessing_ = true;
        cancelRequested_ = false; // Reset flag before starting
        appendLog("Starting processing...");

        // Set FFmpeg threads before processing
        set_ffmpeg_threads(settings.ffmpegThreads);
        appendLog("FFmpeg decode threads: " + QString::number(settings.ffmpegThreads));

        // Save settings persistently
        settingsDialog_->saveSettings();

        refreshQueueUi();
        startNextQueuedItem();
    }
}

void MainWindow::appendLog(const QString& message) {
    logOutput_->append(message);
    qInfo().noquote() << message;
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
