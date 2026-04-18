#pragma once

#include <QMainWindow>
#include <QThread>
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

namespace aa {

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

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief Executes video processing in headless (CLI) mode without starting the GUI.
     * 
     * @param videoPath Semicolon-separated list of video file paths to process.
     * @param pgnOverride Override flag for PGN generation (-1 to use saved settings).
     * @param stockfishOverride Override flag for Stockfish analysis (-1 to use saved settings).
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
    int processHeadless(const QString& videoPath, int pgnOverride = -1, int stockfishOverride = -1, int multiPv = 0, int threads = 0, int depth = 0, int analysisDepth = 0, const QString& debugLevelStr = "", const QString& outputOverride = "", const QString& boardAssetOverride = "", int memoryLimit = -1);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void browseVideo();
    void moveSelectedVideosUp();
    void moveSelectedVideosDown();
    void removeSelectedVideos();
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
    QWidget* createQueueItemWidget(QListWidgetItem* item) const;
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
    QPushButton* removeSelectedBtn_;
    QPushButton* clearQueueBtn_;
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

    // Static Configuration
    static const char* SETTINGS_ORG;
    static const char* SETTINGS_APP;
};

} // namespace aa
