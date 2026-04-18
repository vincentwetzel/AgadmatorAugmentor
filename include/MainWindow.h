#pragma once

#include <QMainWindow>
#include <QThread>
#include <atomic>
#include <QString>
#include "ProcessingSettings.h"
#include "VideoProcessorWorker.h"

class QLineEdit;
class QTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QComboBox;
class QRadioButton;
class QGroupBox;
class ToggleSwitch;

namespace aa {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    int processHeadless(const QString& videoPath, int pgnOverride = -1, int stockfishOverride = -1, int multiPv = 0, int threads = 0, int depth = 0, int analysisDepth = 0, const QString& debugLevelStr = "", const QString& outputOverride = "", const QString& boardAssetOverride = "");

private slots:
    void browseVideo();
    void browseStockfish();
    void autoFindStockfish();
    void onStartCancelClicked();
    void appendLog(const QString& message);
    void updateProgress(int percentage);
    void processingFinished();
    void processingError(const QString& errorMessage);
    void togglePgnExport(bool checked);
    void toggleStockfish(bool checked);
    void onThreadsChanged(int value);
    void onThemeChanged(int index);

private:
    // UI Setup & Management
    void setupUi();
    void setupWorker();
    void loadSettings();
    void saveSettings();
    
    // Settings Management
    ProcessingSettings gatherSettings() const;
    void applySettingsToUi(const ProcessingSettings& settings);
    void applyTheme();
    void updateSettingsButtonIcon();
    void ensureStockfishSettingsVisible();

    // UI Widget Pointers
    QLineEdit* videoPathEdit_;
    QPushButton* browseBtn_;
    QPushButton* settingsBtn_;
    QPushButton* startCancelBtn_;
    QTextEdit* logOutput_;
    QProgressBar* progressBar_;
    QSpinBox* threadSpinBox_;
    
    ToggleSwitch* pgnExportToggle_;
    ToggleSwitch* stockfishToggle_;
    ToggleSwitch* analysisVideoToggle_;
    QComboBox* multiPvComboBox_;
    QComboBox* themeComboBox_;
    QComboBox* debugLevelComboBox_;
    QGroupBox* stockfishSettingsGroup_;

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