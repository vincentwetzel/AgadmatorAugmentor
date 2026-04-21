#pragma once

#include <QDialog>
#include "ProcessingSettings.h"
#include "VideoOverlayConfig.h"

class QLineEdit;
class QSpinBox;
class QComboBox;
class QGroupBox;
class QRadioButton;

class ToggleSwitch;

namespace aa {

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
    void applyHeadlessOverrides(int pgnOverride, int stockfishOverride, int multiPv, int threads, int depth, int analysisDepth, const QString& debugLevelStr, int memoryLimit);

signals:
    /// @brief Emitted when the settings dialog needs to append a message to the main application log.
    void logMessage(const QString& msg);
    
    /// @brief Emitted when the application UI theme is changed by the user.
    void themeChanged();

private:
    void setupUi();

    ToggleSwitch* pgnExportToggle_;
    ToggleSwitch* stockfishToggle_;
    ToggleSwitch* analysisVideoToggle_;
    QComboBox* multiPvComboBox_;
    QComboBox* themeComboBox_;
    QComboBox* debugLevelComboBox_;
    QSpinBox* threadSpinBox_;
    QGroupBox* stockfishSettingsGroup_;
    VideoOverlayConfig currentOverlayConfig_;
};

} // namespace aa