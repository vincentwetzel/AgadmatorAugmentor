#include "MainWindow.h"
#include "TemplateManager.h"

#include <QListWidget>
#include <QListWidgetItem>
#include <QVariantMap>
#include <QFileInfo>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QProgressBar>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QResizeEvent>

namespace {

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

class QueueItemWidget : public QFrame {
public:
    QListWidgetItem* item;
    QueueItemWidget(QListWidgetItem* i) : item(i) {}
    void resizeEvent(QResizeEvent* event) override {
        QFrame::resizeEvent(event);
        int h = heightForWidth(width());
        if (h > 0 && item->sizeHint().height() != h) {
            item->setSizeHint(QSize(10, h));
        }
    }
};

} // namespace

namespace cta {

QVariantMap overlayConfigToVariantMap(const VideoOverlayConfig& config);
VideoOverlayConfig overlayConfigFromVariantMap(const QVariant& value);

MainWindow::QueueItemStatus MainWindow::itemStatus(const QListWidgetItem* item) const {
    if (!item) return QueueItemStatus::Queued;
    return static_cast<QueueItemStatus>(item->data(StatusRole).toInt());
}

void MainWindow::setItemStatus(QListWidgetItem* item, QueueItemStatus status) {
    if (!item) return;
    if (itemStatus(item) != status) {
        item->setData(StatusRole, static_cast<int>(status));
        refreshQueueItem(item);
    }
}

void MainWindow::setItemProgress(QListWidgetItem* item, int percentage) {
    if (!item) return;
    int bounded = qBound(0, percentage, 100);
    if (item->data(ProgressRole).toInt() != bounded) {
        item->setData(ProgressRole, bounded);
        refreshQueueItem(item);
    }
}

void MainWindow::applyTemplateToItem(QListWidgetItem* item, const QString& templateId) const {
    if (!item) return;
    auto optTpl = cta::TemplateManager::instance().getTemplate(templateId);
    const auto tpl = optTpl.has_value() ? optTpl.value() : cta::TemplateManager::instance().getFallbackTemplate();

    item->setData(TemplateRole, tpl.id);
    item->setData(TemplateNameRole, tpl.name);
    item->setData(TemplateConfigRole, cta::overlayConfigToVariantMap(tpl.config));
}

VideoOverlayConfig MainWindow::overlayConfigForItem(const QListWidgetItem* item) const {
    if (!item) return cta::TemplateManager::instance().getFallbackTemplate().config;

    const QVariant storedConfig = item->data(TemplateConfigRole);
    if (storedConfig.isValid()) {
        return cta::overlayConfigFromVariantMap(storedConfig);
    }

    const QString templateId = item->data(TemplateRole).toString();
    auto optTpl = cta::TemplateManager::instance().getTemplate(templateId);
    if (optTpl.has_value()) {
        return optTpl->config;
    }
    return cta::TemplateManager::instance().getFallbackTemplate().config;
}

QString MainWindow::templateNameForItem(const QListWidgetItem* item) const {
    if (!item) return cta::TemplateManager::instance().getFallbackTemplate().name;

    const QString storedName = item->data(TemplateNameRole).toString();
    if (!storedName.isEmpty()) {
        return storedName;
    }

    const QString templateId = item->data(TemplateRole).toString();
    auto optTpl = cta::TemplateManager::instance().getTemplate(templateId);
    if (optTpl.has_value()) {
        return optTpl->name;
    }
    return cta::TemplateManager::instance().getFallbackTemplate().name;
}

QWidget* MainWindow::createQueueItemWidget(QListWidgetItem* item) {
    const QString path = item->data(PathRole).toString();
    const QString fileName = QFileInfo(path).fileName();
    const QueueItemStatus status = itemStatus(item);
    const int progress = item->data(ProgressRole).toInt();
    const QString outputDir = item->data(OutputDirRole).toString();
    const QString templateId = item->data(TemplateRole).toString();

    auto* container = new QueueItemWidget(item);
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

    auto* removeBtn = new QPushButton("Remove");
    removeBtn->setObjectName("removeQueueItemBtn");
    removeBtn->setToolTip(status == QueueItemStatus::Processing
        ? "The video currently being processed cannot be removed."
        : "Remove this video from the queue.");
    removeBtn->setEnabled(status != QueueItemStatus::Processing);
    connect(removeBtn, &QPushButton::clicked, container, [this, item]() {
        removeQueueItem(item);
    });
    topRow->addWidget(removeBtn, 0, Qt::AlignRight);
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
            lastUsedTemplateId_ = newId;
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
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(OutputDirRole).toString()));
    });
    progressRow->addWidget(openFolderBtn);

    layout->addLayout(progressRow);

    return container;
}

void MainWindow::refreshQueueItem(QListWidgetItem* item) {
    if (!item) return;

    const QString path = item->data(PathRole).toString();
    const QueueItemStatus status = itemStatus(item);
    int progress = item->data(ProgressRole).toInt();

    if (status == QueueItemStatus::Completed) {
        progress = 100;
        if (item->data(ProgressRole).toInt() != progress) {
            item->setData(ProgressRole, progress);
        }
    } else if ((status == QueueItemStatus::Failed || status == QueueItemStatus::Cancelled) && progress == 100) {
        progress = 0;
        if (item->data(ProgressRole).toInt() != progress) {
            item->setData(ProgressRole, progress);
        }
    }

    item->setToolTip(path + "\nStatus: " + queueStatusText(status));

    if (auto* existingWidget = queueList_->itemWidget(item)) {
        existingWidget->setToolTip(path + "\nStatus: " + queueStatusText(status));

        if (auto* statusLabel = existingWidget->findChild<QLabel*>("statusLabel")) {
            statusLabel->setText(queueStatusText(status));
        }
        if (auto* removeBtn = existingWidget->findChild<QPushButton*>("removeQueueItemBtn")) {
            const bool removable = status != QueueItemStatus::Processing;
            removeBtn->setEnabled(removable);
            removeBtn->setToolTip(removable
                ? "Remove this video from the queue."
                : "The video currently being processed cannot be removed.");
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
            const QString outputDir = item->data(OutputDirRole).toString();
            openFolderBtn->setVisible(status == QueueItemStatus::Completed && !outputDir.isEmpty());
            openFolderBtn->setEnabled(status == QueueItemStatus::Completed && !outputDir.isEmpty());
        }
        if (auto* tplCombo = existingWidget->findChild<QComboBox*>("queueTemplateCombo")) {
            const QString templateId = item->data(TemplateRole).toString();
            tplCombo->blockSignals(true);
            tplCombo->clear();
            const auto templates = cta::TemplateManager::instance().getAllTemplates();
            int idx = 0;
            for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
                tplCombo->addItem(templates[i].name, templates[i].id);
                if (templates[i].id == templateId) idx = i;
            }
            if (tplCombo->count() > 0) {
                applyTemplateToItem(item, tplCombo->itemData(idx).toString());
                tplCombo->setCurrentIndex(idx);
            }
            tplCombo->setEnabled(status == QueueItemStatus::Queued);
            tplCombo->blockSignals(false);
        }
        
        int w = queueList_->viewport()->width();
        if (w < 100) w = 400; // Safe fallback before layout
        int h = existingWidget->heightForWidth(w);
        QSize newSizeHint(10, h > 0 ? h : existingWidget->sizeHint().height());
        if (item->sizeHint() != newSizeHint) {
            item->setSizeHint(newSizeHint);
        }
    } else {
        QWidget* newWidget = createQueueItemWidget(item);
        int w = queueList_->viewport()->width();
        if (w < 100) w = 400;
        int h = newWidget->heightForWidth(w);
        item->setSizeHint(QSize(10, h > 0 ? h : newWidget->sizeHint().height()));
        queueList_->setItemWidget(item, newWidget);
    }
}

QListWidgetItem* MainWindow::findQueueItemByPath(const QString& path) const {
    for (int i = 0; i < queueList_->count(); ++i) {
        auto* item = queueList_->item(i);
        if (item->data(PathRole).toString().compare(path, Qt::CaseInsensitive) == 0) {
            return item;
        }
    }
    return nullptr;
}

QListWidgetItem* MainWindow::nextQueuedItem() const {
    for (int i = 0; i < queueList_->count(); ++i) {
        auto* item = queueList_->item(i);
        if (itemStatus(item) == QueueItemStatus::Queued) return item;
    }
    return nullptr;
}

bool MainWindow::hasQueuedItems() const {
    return nextQueuedItem() != nullptr;
}

bool MainWindow::hasRemovableItems() const {
    for (int i = 0; i < queueList_->count(); ++i) {
        if (itemStatus(queueList_->item(i)) != QueueItemStatus::Processing) return true;
    }
    return false;
}

bool MainWindow::canMoveSelectionUp() const {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) return false;

    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) return false;
        const int row = queueList_->row(item);
        if (row > 0 && itemStatus(queueList_->item(row - 1)) != QueueItemStatus::Processing) return true;
    }
    return false;
}

bool MainWindow::canMoveSelectionDown() const {
    const auto selectedItems = queueList_->selectedItems();
    if (selectedItems.isEmpty()) return false;

    for (auto* item : selectedItems) {
        if (itemStatus(item) == QueueItemStatus::Processing) return false;
        const int row = queueList_->row(item);
        if (row >= 0 && row < queueList_->count() - 1 && itemStatus(queueList_->item(row + 1)) != QueueItemStatus::Processing) return true;
    }
    return false;
}

void MainWindow::refreshQueueUi() {
    const int count = queueList_ ? queueList_->count() : 0;
    const bool hasItems = count > 0;

    if (queueEmptyStateLabel_) queueEmptyStateLabel_->setVisible(!hasItems);

    clearQueueBtn_->setEnabled(hasItems && hasRemovableItems());
    moveUpBtn_->setEnabled(hasItems && canMoveSelectionUp());
    moveDownBtn_->setEnabled(hasItems && canMoveSelectionDown());
    browseBtn_->setEnabled(true);

    if (isProcessing_) {
        startCancelBtn_->setText("Cancel Current");
        startCancelBtn_->setEnabled(true);
        queueHelperLabel_->setText("Queue is live: add more videos while processing. Use a row's Remove button, or select non-processing items and press Delete.");
    } else {
        startCancelBtn_->setText("Start Processing");
        startCancelBtn_->setEnabled(hasQueuedItems());
        queueHelperLabel_->setText("Drag and drop video files into the queue below, or click Add Video(s)...");
    }
}

} // namespace cta
