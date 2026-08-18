#pragma once

#include <QMainWindow>
#include <QThread>
#include <QElapsedTimer>
#include <atomic>
#include <QString>
#include "ProcessingSettings.h"
#include "VideoProcessorWorker.h"

class QTextEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QShortcut;
class QWidget;
class QDragEnterEvent;
class QDropEvent;

namespace cta {

class SettingsDialog;

/**
 * @class MainWindow
 * @brief The primary application window for ChessTube Analyzer.
 *
 * Manages the main user interface, delegates configuration to the SettingsDialog,
 * and orchestrates video processing tasks via a dedicated worker thread. 
 * Supports both standard GUI and headless batch processing modes.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    enum class QueueItemStatus {
        Queued = 0,
        Processing = 1,
        Completed = 2,
        Failed = 3,
        Cancelled = 4
    };

    static constexpr int PathRole = Qt::UserRole;
    static constexpr int StatusRole = Qt::UserRole + 1;
    static constexpr int ProgressRole = Qt::UserRole + 2;
    static constexpr int OutputDirRole = Qt::UserRole + 3;
    static constexpr int TemplateRole = Qt::UserRole + 4;
    static constexpr int TemplateNameRole = Qt::UserRole + 5;
    static constexpr int TemplateConfigRole = Qt::UserRole + 6;

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief Executes video processing in headless (CLI) mode without starting the GUI.
     * 
     * @param videoPath Semicolon-separated list of video file paths to process.
     * @param pgnOverride Override flag for PGN generation (-1 to use saved settings).
     * @param analysisVideoOverride Override flag for analysis video generation (-1 to use saved settings).
     * @param moveLabelsOverride Override flag for move quality labels (-1 to use saved settings).
     * @param multiPv Override for the number of principal variations to compute.
     * @param threads Override for the number of FFmpeg decode threads.
     * @param depth Override for Stockfish search depth.
     * @param analysisDepth Override for the engine variation length.
     * @param debugLevelStr Override for the debug image generation level ("NONE", "MOVES", "FULL").
     * @param outputOverride Custom output directory or specific file path.
     * @param boardAssetOverride Custom path to the board template asset.
     * @param memoryLimit Limit the maximum RAM usage for analysis buffers in MB.
     * 
     * @return int 0 on success, non-zero on failure.
     */
    int processHeadless(const QString& videoPath, int pgnOverride = -1, int analysisVideoOverride = -1, int moveLabelsOverride = -1, int multiPv = 0, int threads = 0, int depth = 0, int time = 0, int nodes = 0, int analysisDepth = 0, const QString& debugLevelStr = "", const QString& outputOverride = "", const QString& boardAssetOverride = "", int memoryLimit = -1);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void browseVideo();
    void moveSelectedVideosUp();
    void moveSelectedVideosDown();
    void removeSelectedVideos();
    void removeQueueItem(QListWidgetItem* item);
    void clearQueue();
    void onStartCancelClicked();
    void appendLog(const QString& message);
    void updateProgress(int percentage);
    void processingFinished();
    void processingError(const QString& errorMessage);
    void applyTheme();

private:
    // UI Setup & Management
    void setupUi();
    void setupWorker();
    void addVideosToQueue(const QStringList& paths);
    void refreshQueueUi();
    void refreshQueueItem(QListWidgetItem* item);
    QWidget* createQueueItemWidget(QListWidgetItem* item);
    QListWidgetItem* findQueueItemByPath(const QString& path) const;
    QListWidgetItem* nextQueuedItem() const;
    bool hasQueuedItems() const;
    bool hasRemovableItems() const;
    bool canMoveSelectionUp() const;
    bool canMoveSelectionDown() const;
    void startProcessingItem(QListWidgetItem* item);
    void startNextQueuedItem();
    void finishProcessingSession();
    QueueItemStatus itemStatus(const QListWidgetItem* item) const;
    void setItemStatus(QListWidgetItem* item, QueueItemStatus status);
    void setItemProgress(QListWidgetItem* item, int percentage);
    void applyTemplateToItem(QListWidgetItem* item, const QString& templateId) const;
    VideoOverlayConfig overlayConfigForItem(const QListWidgetItem* item) const;
    QString templateNameForItem(const QListWidgetItem* item) const;
    
    // Settings Management
    ProcessingSettings gatherSettings() const;
    void updateSettingsButtonIcon();

    // UI Widget Pointers
    QListWidget* queueList_;
    QLabel* queueEmptyStateLabel_;
    QLabel* queueHelperLabel_;
    QShortcut* deleteSelectionShortcut_;
    QPushButton* browseBtn_;
    QPushButton* moveUpBtn_;
    QPushButton* moveDownBtn_;
    QPushButton* clearQueueBtn_;
    QPushButton* templatesBtn_;
    QPushButton* settingsBtn_;
    QPushButton* startCancelBtn_;
    QTextEdit* logOutput_;

    SettingsDialog* settingsDialog_;

    // Worker Thread Management
    QThread workerThread_;
    VideoProcessorWorker* worker_;
    
    // State
    bool isProcessing_ = false;
    std::atomic<bool> cancelRequested_{false};
    QElapsedTimer logTimer_;

    // Memory for the template fallback
    mutable QString lastUsedTemplateId_;

    // Static Configuration
    static const char* SETTINGS_ORG;
    static const char* SETTINGS_APP;
};

} // namespace cta
