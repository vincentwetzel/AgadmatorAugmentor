#include "MainWindow.h"
#include "TemplateManager.h"

#include <QFileInfo>
#include <QListWidget>
#include <QListWidgetItem>
#include <algorithm>

namespace cta {

void MainWindow::addVideosToQueue(const QStringList& paths) {
    QStringList addedNames;

    for (const QString& rawPath : paths) {
        const QString localPath = QFileInfo(rawPath).absoluteFilePath();
        QFileInfo info(localPath);
        if (!info.exists() || !info.isFile()) continue;

        const QString suffix = info.suffix().toLower();
        if (suffix != "mp4" && suffix != "mkv" && suffix != "avi" && suffix != "mov" && suffix != "webm") continue;
        if (findQueueItemByPath(localPath) != nullptr) continue;

        auto* item = new QListWidgetItem();
        item->setData(PathRole, localPath);
        item->setData(StatusRole, static_cast<int>(QueueItemStatus::Queued));
        item->setData(ProgressRole, 0);
        
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
        if (isProcessing_) appendLog("New videos will start automatically after the current batch item finishes.");
    }
    refreshQueueUi();
}

void MainWindow::moveSelectedVideosUp() {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) return;

    QList<int> rows;
    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) continue;
        rows.append(queueList_->row(item));
    }
    std::sort(rows.begin(), rows.end());

    bool moved = false;
    for (int row : rows) {
        if (row <= 0) continue;
        auto* aboveItem = queueList_->item(row - 1);
        if (!aboveItem || aboveItem->isSelected() || itemStatus(aboveItem) == QueueItemStatus::Processing) continue;

        auto* item = queueList_->takeItem(row);
        queueList_->insertItem(row - 1, item);
        item->setSelected(true);
        moved = true;
    }
    if (moved) appendLog("Moved selected queue item(s) up.");
    refreshQueueUi();
}

void MainWindow::moveSelectedVideosDown() {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) return;

    QList<int> rows;
    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) continue;
        rows.append(queueList_->row(item));
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    bool moved = false;
    for (int row : rows) {
        if (row < 0 || row >= queueList_->count() - 1) continue;
        auto* belowItem = queueList_->item(row + 1);
        if (!belowItem || belowItem->isSelected() || itemStatus(belowItem) == QueueItemStatus::Processing) continue;

        auto* item = queueList_->takeItem(row);
        queueList_->insertItem(row + 1, item);
        item->setSelected(true);
        moved = true;
    }
    if (moved) appendLog("Moved selected queue item(s) down.");
    refreshQueueUi();
}

void MainWindow::removeSelectedVideos() {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) return;

    int removedCount = 0;
    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) continue;
        delete queueList_->takeItem(queueList_->row(item));
        ++removedCount;
    }
    if (removedCount > 0) appendLog("Removed " + QString::number(removedCount) + " queue item(s).");
    refreshQueueUi();
}

void MainWindow::removeQueueItem(QListWidgetItem* item) {
    if (!item || itemStatus(item) == QueueItemStatus::Processing) return;

    const int row = queueList_->row(item);
    if (row < 0) return;

    const QString fileName = QFileInfo(item->data(PathRole).toString()).fileName();
    delete queueList_->takeItem(row);
    appendLog("Removed \"" + fileName + "\" from the queue.");
    refreshQueueUi();
}

void MainWindow::clearQueue() {
    if (queueList_->count() == 0) return;

    for (int i = queueList_->count() - 1; i >= 0; --i) {
        auto* item = queueList_->item(i);
        if (itemStatus(item) == QueueItemStatus::Processing) continue;
        delete queueList_->takeItem(i);
    }
    appendLog(isProcessing_ ? "Cleared all non-processing queue items." : "Cleared the video queue.");
    refreshQueueUi();
}

} // namespace cta
