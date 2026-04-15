#pragma once

#include <QMainWindow>
#include <QThread>
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
class ToggleSwitch;

namespace aa {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    int processHeadless(const QString& videoPath);

private slots:
    void browseVideo();
    void startProcessing();
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

    // UI Widget Pointers
    QLineEdit* videoPathEdit_;
    QPushButton* browseBtn_;
    QPushButton* startBtn_;
    QTextEdit* logOutput_;
    QProgressBar* progressBar_;
    QSpinBox* threadSpinBox_;
    
    ToggleSwitch* pgnExportToggle_;
    ToggleSwitch* stockfishToggle_;
    QComboBox* multiPvComboBox_;
    QComboBox* themeComboBox_;

    // Worker Thread Management
    QThread workerThread_;
    VideoProcessorWorker* worker_;
    
    // State
    bool isProcessing_ = false;

    // Static Configuration
    static const char* SETTINGS_ORG;
    static const char* SETTINGS_APP;
};

} // namespace aa