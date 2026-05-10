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
#include <QRegularExpression>
#include <QtMath>
#include <algorithm>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

QString formatElapsedPrefix(qint64 totalMs) {
    const qint64 h = totalMs / 3600000;
    const qint64 m = (totalMs % 3600000) / 60000;
    const qint64 s = (totalMs % 60000) / 1000;
    const qint64 ms = totalMs % 1000;

    if (h > 0) {
        return QString("[%1:%2:%3.%4]")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'))
            .arg(ms, 3, 10, QChar('0'));
    }

    return QString("[%1:%2.%3]")
        .arg(m)
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

bool hasElapsedPrefix(const QString& line) {
    static const QRegularExpression elapsedPrefixPattern(
        QStringLiteral("^\\s*\\[(?:\\d+:)?\\d+:\\d{2}\\.\\d{3}\\]"));
    return elapsedPrefixPattern.match(line).hasMatch();
}

} // namespace

namespace cta {

const char* MainWindow::SETTINGS_ORG = "ChessTubeAnalyzer";
const char* MainWindow::SETTINGS_APP = "settings";

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    QCoreApplication::setOrganizationName(SETTINGS_ORG);
    QCoreApplication::setApplicationName(SETTINGS_ORG);

    setWindowTitle("ChessTube Analyzer");
    resize(800, 600);
    logTimer_.start();

    TemplateManager::instance().initialize();

    setupUi();
    setupWorker();
    settingsDialog_->loadSettings(); // Load all settings from INI file
    applyTheme();
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::browseVideo() {
    QSettings settings;
    QString lastDir = settings.value("lastVideoDir", QDir::homePath()).toString();

    QStringList fileNames = QFileDialog::getOpenFileNames(this, "Select Chess Video(s)", lastDir, "Video Files (*.mp4 *.mkv *.avi);;All Files (*)");
    if (!fileNames.isEmpty()) {
        addVideosToQueue(fileNames);
        settings.setValue("lastVideoDir", QFileInfo(fileNames.first()).absolutePath());
        settings.sync();
    }
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
        QSettings settings;
        settings.setValue("lastVideoDir", QFileInfo(droppedFiles.first()).absolutePath());
        settings.sync();
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::appendLog(const QString& message) {
    const QString prefix = formatElapsedPrefix(logTimer_.elapsed());
    const QStringList lines = message.split('\n');
    QStringList formattedLines;
    formattedLines.reserve(lines.size());

    for (const QString& line : lines) {
        if (line.isEmpty() || hasElapsedPrefix(line)) {
            formattedLines << line;
        } else {
            formattedLines << prefix + " " + line;
        }
    }

    const QString formattedMessage = formattedLines.join('\n');
    logOutput_->append(formattedMessage);
    qInfo().noquote() << formattedMessage;
}

} // namespace cta
