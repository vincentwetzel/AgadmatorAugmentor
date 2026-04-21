#include "MainWindow.h"
#include "ToggleSwitch.h"
#include "ThemeManager.h"
#include "SettingsDialog.h"
#include "OverlayEditorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QRadioButton>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QSizePolicy>
#include <QSettings>
#include <QDir>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSize>
#include <QtMath>
#include <QApplication>
#include <QAbstractItemView>
#include <QStackedLayout>
#include <QShortcut>
#include <QKeySequence>

namespace {

QIcon createSettingsCogIcon(const QColor& color) {
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

} // namespace

namespace aa {

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    setAcceptDrops(true);

    auto* rootLayout = new QVBoxLayout(centralWidget);

    // Top layout (Queue summary + controls)
    auto* fileLayout = new QHBoxLayout();
    browseBtn_ = new QPushButton("Add Video(s)...");
    browseBtn_->setToolTip("Open a file dialog and add one or more video files to the queue");
    fileLayout->addWidget(browseBtn_);

    moveUpBtn_ = new QPushButton("Move Up");
    moveUpBtn_->setToolTip("Move the selected queue items up by one position. The currently processing item cannot be moved.");
    fileLayout->addWidget(moveUpBtn_);

    moveDownBtn_ = new QPushButton("Move Down");
    moveDownBtn_->setToolTip("Move the selected queue items down by one position. The currently processing item cannot be moved.");
    fileLayout->addWidget(moveDownBtn_);

    removeSelectedBtn_ = new QPushButton("Remove Selected");
    removeSelectedBtn_->setToolTip("Remove the selected queue entries, except for the video that is currently processing");
    fileLayout->addWidget(removeSelectedBtn_);

    clearQueueBtn_ = new QPushButton("Clear Queue");
    clearQueueBtn_->setToolTip("Remove all queue entries except for the video that is currently processing");
    fileLayout->addWidget(clearQueueBtn_);

    templatesBtn_ = new QPushButton("Manage Templates");
    templatesBtn_->setToolTip("Manage channel-specific analysis overlay templates");
    fileLayout->addWidget(templatesBtn_);

    settingsBtn_ = new QPushButton();
    settingsBtn_->setObjectName("settingsBtn");
    settingsBtn_->setText("");
    settingsBtn_->setToolTip("Open Settings");
    settingsBtn_->setCursor(Qt::PointingHandCursor);
    settingsBtn_->setFixedSize(32, 32);
    settingsBtn_->setIconSize(QSize(20, 20));
    fileLayout->addWidget(settingsBtn_);

    rootLayout->addLayout(fileLayout);

    queueHelperLabel_ = new QLabel("Drag and drop video files into the queue below, or click Add Video(s). Completed and failed items stay visible here.");
    queueHelperLabel_->setWordWrap(true);
    queueHelperLabel_->setToolTip("Shows the two supported ways to add videos to the queue: drag and drop files into the queue area, or browse for them.");
    rootLayout->addWidget(queueHelperLabel_);

    queueList_ = new QListWidget();
    queueList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    queueList_->setAlternatingRowColors(true);
    queueList_->setToolTip("Queued videos. Drag files from File Explorer onto this queue, or use Add Video(s). Statuses stay visible here while the batch runs. Select non-processing entries and press Delete to remove them.");
    queueList_->setMinimumHeight(170);

    auto* queueAreaWidget = new QWidget();
    queueAreaWidget->setToolTip("Drop video files here to add them to the queue, or use the Add Video(s) button.");
    auto* queueAreaLayout = new QStackedLayout(queueAreaWidget);
    queueAreaLayout->setContentsMargins(0, 0, 0, 0);
    queueAreaLayout->setStackingMode(QStackedLayout::StackAll);

    queueEmptyStateLabel_ = new QLabel("+\n\nDrag and drop videos here\nor click Add Video(s)...\n\nQueued, processing, completed, and failed items will appear here.");
    queueEmptyStateLabel_->setAlignment(Qt::AlignCenter);
    queueEmptyStateLabel_->setWordWrap(true);
    queueEmptyStateLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    queueEmptyStateLabel_->setToolTip("Appears when the queue is empty to show that this area accepts dragged video files.");

    queueAreaLayout->addWidget(queueList_);
    queueAreaLayout->addWidget(queueEmptyStateLabel_);
    rootLayout->addWidget(queueAreaWidget);

    deleteSelectionShortcut_ = new QShortcut(QKeySequence::Delete, queueList_);
    deleteSelectionShortcut_->setContext(Qt::WidgetShortcut);
    deleteSelectionShortcut_->setAutoRepeat(false);

    settingsDialog_ = new SettingsDialog(this);

    // Log output
    logOutput_ = new QTextEdit();
    logOutput_->setReadOnly(true);
    logOutput_->setToolTip("Processing log output showing progress and messages");
    logOutput_->setMinimumHeight(140);
    rootLayout->addWidget(logOutput_);

    // Start row
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();
    startCancelBtn_ = new QPushButton("Start Processing");
    startCancelBtn_->setToolTip("Begin processing queued videos one after another, or cancel the current video while leaving the rest of the queue intact");
    bottomLayout->addWidget(startCancelBtn_);
    bottomLayout->addStretch();
    rootLayout->addLayout(bottomLayout);

    connect(browseBtn_, &QPushButton::clicked, this, &MainWindow::browseVideo);
    connect(moveUpBtn_, &QPushButton::clicked, this, &MainWindow::moveSelectedVideosUp);
    connect(moveDownBtn_, &QPushButton::clicked, this, &MainWindow::moveSelectedVideosDown);
    connect(removeSelectedBtn_, &QPushButton::clicked, this, &MainWindow::removeSelectedVideos);
    connect(clearQueueBtn_, &QPushButton::clicked, this, &MainWindow::clearQueue);
    connect(deleteSelectionShortcut_, &QShortcut::activated, this, &MainWindow::removeSelectedVideos);
    connect(startCancelBtn_, &QPushButton::clicked, this, &MainWindow::onStartCancelClicked);
    connect(queueList_, &QListWidget::itemSelectionChanged, this, &MainWindow::refreshQueueUi);
    
    connect(templatesBtn_, &QPushButton::clicked, this, [this]() {
        OverlayEditorDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            refreshQueueItem(queueList_->currentItem()); // Refresh UI incase active template was modified
        }
    });
    connect(settingsBtn_, &QPushButton::clicked, settingsDialog_, &QDialog::exec);
    connect(settingsDialog_, &SettingsDialog::logMessage, this, &MainWindow::appendLog);
    connect(settingsDialog_, &SettingsDialog::themeChanged, this, &MainWindow::applyTheme);

    refreshQueueUi();
}

void MainWindow::applyTheme() {
    QSettings settings;
    int themeIndex = settings.value("themeMode", 0).toInt();
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

} // namespace aa
