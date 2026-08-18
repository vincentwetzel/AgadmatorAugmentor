#pragma once

#include <QDialog>
#include "ProcessingSettings.h"
#include "VideoOverlayConfig.h"

class QLineEdit;
class QSpinBox;
class QComboBox;
class QGroupBox;
class QRadioButton;
class QCheckBox;
class QPushButton;

class ToggleSwitch;

namespace cta {

/**
 * @class SettingsDialog
 * @brief Dialog window for configuring application settings.
 *
 * Manages UI elements for video export options, Stockfish analysis settings,
 * and advanced performance configurations. It reads from and writes to 
 * a persistent 'settings.ini' configuration file.
 */
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    /**
     * @brief Loads application settings from the settings.ini file and populates the UI.
     */
    void loadSettings();

    /**
     * @brief Saves the current UI state to the persistent settings.ini file.
     */
    void saveSettings();

    /**
     * @brief Applies an existing ProcessingSettings struct directly to the UI elements.
     * @param settings The ProcessingSettings struct to apply.
     */
    void applySettingsToUi(const ProcessingSettings& settings);

    /**
     * @brief Populates a ProcessingSettings struct based on the current UI state.
     * @param s The ProcessingSettings struct to populate.
     */
    void populateSettings(ProcessingSettings& s) const;

    /**
     * @brief Applies CLI-provided overrides directly to the active configuration state.
     */
    void applyHeadlessOverrides(int pgnOverride, int analysisVideoOverride, int moveLabelsOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, int memoryLimit);

signals:
    /// @brief Emitted when the settings dialog needs to append a message to the main application log.
    void logMessage(const QString& msg);
    
    /// @brief Emitted when the application UI theme is changed by the user.
    void themeChanged();

private:
    void setupUi();
    void connectAutoSaveSignals();

    struct Ui {
        ToggleSwitch* pgnExportToggle = nullptr;
        ToggleSwitch* pgnAnalysisToggle = nullptr;
        ToggleSwitch* pgnAnnotationsToggle = nullptr;
        ToggleSwitch* subtitlesToggle = nullptr;
        ToggleSwitch* analysisVideoToggle = nullptr;
        ToggleSwitch* videoAnnotationsToggle = nullptr;
        ToggleSwitch* removeOriginalToggle = nullptr;
        ToggleSwitch* fastPreviewToggle = nullptr;

        QComboBox* multiPvComboBox = nullptr;
        QComboBox* themeComboBox = nullptr;
        QComboBox* debugLevelComboBox = nullptr;
        QComboBox* threadComboBox = nullptr;
        QComboBox* depthComboBox = nullptr;
        QComboBox* timeComboBox = nullptr;
        QComboBox* nodesComboBox = nullptr;
        QComboBox* analysisDepthComboBox = nullptr;
        QComboBox* videoCodecComboBox = nullptr;
        QComboBox* audioCodecComboBox = nullptr;
        QComboBox* extensionComboBox = nullptr;
        QComboBox* resolutionComboBox = nullptr;
        QComboBox* qualityComboBox = nullptr;
        QComboBox* memoryLimitComboBox = nullptr;

        QLineEdit* stockfishPathEdit = nullptr;
        QPushButton* stockfishPathBtn = nullptr;
        QPushButton* stockfishSearchBtn = nullptr;

        QGroupBox* stockfishSettingsGroup = nullptr;

        QLineEdit* defaultVideoDirEdit = nullptr;
        QRadioButton* sameAsSourceRadio = nullptr;
        QRadioButton* customDirRadio = nullptr;
        QLineEdit* customDirEdit = nullptr;
        
        QSpinBox* logRetentionSpinBox = nullptr;
        QCheckBox* compressLogsCheck = nullptr;
    } ui;

    VideoOverlayConfig currentOverlayConfig_;
};

} // namespace cta
