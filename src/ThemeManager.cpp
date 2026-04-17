// Extracted from cpp directory
#include "ThemeManager.h"

#ifdef _WIN32
#include <windows.h>
#include <winreg.h>
#endif

namespace aa {

const char* ThemeManager::SETTINGS_ORG = "ChessTubeAnalyzer";
const char* ThemeManager::SETTINGS_APP = "ChessTubeAnalyzer";
const char* ThemeManager::SETTINGS_THEME_KEY = "themeMode";

ThemeManager::ThemeManager() : currentTheme_(ThemeMode::System), settings_(nullptr) {
    settings_ = new QSettings(SETTINGS_ORG, SETTINGS_APP);
    loadSettings();
}

ThemeManager& ThemeManager::instance() {
    static ThemeManager instance;
    return instance;
}

void ThemeManager::loadSettings() {
    int themeValue = settings_->value(SETTINGS_THEME_KEY, static_cast<int>(ThemeMode::System)).toInt();
    currentTheme_ = static_cast<ThemeMode>(themeValue);
}

void ThemeManager::saveSettings() const {
    settings_->setValue(SETTINGS_THEME_KEY, static_cast<int>(currentTheme_));
}

void ThemeManager::setTheme(ThemeMode mode) {
    currentTheme_ = mode;
    saveSettings();
}

QString ThemeManager::themeName() const {
    switch (currentTheme_) {
        case ThemeMode::System: return isSystemDarkMode() ? "Dark (System)" : "Light (System)";
        case ThemeMode::Light: return "Light";
        case ThemeMode::Dark: return "Dark";
        default: return "Unknown";
    }
}

ThemeManager::ThemeColors ThemeManager::colors() const {
    bool isDark = (currentTheme_ == ThemeMode::Dark) || 
                  (currentTheme_ == ThemeMode::System && isSystemDarkMode());

    ThemeColors c;
    
    if (isDark) {
        // Dark theme colors
        c.windowBackground = "#1e1e1e";
        c.windowText = "#ffffff";
        c.buttonBackground = "#3d3d3d";
        c.buttonText = "#ffffff";
        c.buttonHoverBackground = "#4d4d4d";
        c.baseBackground = "#2d2d2d";
        c.baseText = "#ffffff";
        c.groupBoxBackground = "#252525";
        c.groupBoxBorder = "#555555";
        c.groupBoxTitle = "#ffffff";
        c.highlight = "#0078d4";
        c.highlightText = "#ffffff";
        c.selectionBackground = "#0078d4";
        c.selectionText = "#ffffff";
        c.progressBarBackground = "#3d3d3d";
        c.progressBarChunk = "#0078d4";
        c.toggleCheckedBackground = "#4CAF50";
        c.toggleUncheckedBackground = "#666666";
        c.toggleThumb = "#ffffff";
    } else {
        // Light theme colors
        c.windowBackground = "#ffffff";
        c.windowText = "#000000";
        c.buttonBackground = "#f0f0f0";
        c.buttonText = "#000000";
        c.buttonHoverBackground = "#e0e0e0";
        c.baseBackground = "#ffffff";
        c.baseText = "#000000";
        c.groupBoxBackground = "#fafafa";
        c.groupBoxBorder = "#cccccc";
        c.groupBoxTitle = "#000000";
        c.highlight = "#0078d4";
        c.highlightText = "#ffffff";
        c.selectionBackground = "#0078d4";
        c.selectionText = "#ffffff";
        c.progressBarBackground = "#e0e0e0";
        c.progressBarChunk = "#0078d4";
        c.toggleCheckedBackground = "#4CAF50";
        c.toggleUncheckedBackground = "#888888";
        c.toggleThumb = "#ffffff";
    }
    
    return c;
}

QString ThemeManager::generateStyleSheet() const {
    auto c = colors();

    QString qss = QString(R"QSS(
        /* ===== UNIVERSAL STYLES - DO NOT OVERRIDE IN INDIVIDUAL COMPONENTS ===== */
        
        /* Global widget defaults */
        QWidget {
            background-color: %1;
            color: %2;
            font-family: "Segoe UI", "Arial", sans-serif;
            font-size: 13px;
        }

        /* Buttons */
        QPushButton {
            background-color: %3;
            color: %4;
            border: 1px solid %9;
            border-radius: 4px;
            padding: 6px 12px;
            min-height: 24px;
        }
        QPushButton:hover {
            background-color: %5;
        }
        QPushButton:pressed {
            background-color: %9;
        }
        QPushButton:disabled {
            background-color: %3;
            color: #888888;
        }
        QPushButton#settingsBtn {
            padding: 0px;
            min-width: 32px;
            max-width: 32px;
            min-height: 32px;
            max-height: 32px;
            text-align: center;
        }

        /* Text input fields */
        QLineEdit, QTextEdit, QPlainTextEdit {
            background-color: %6;
            color: %7;
            border: 1px solid %9;
            border-radius: 4px;
            padding: 4px;
            selection-background-color: %12;
            selection-color: %13;
        }
        QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {
            border: 2px solid %11;
        }
        QLineEdit:disabled, QTextEdit:disabled, QPlainTextEdit:disabled {
            background-color: %3;
            color: #888888;
        }

        /* Radio Buttons */
        QRadioButton {
            background-color: transparent;
            color: %2;
            spacing: 6px;
        }
        QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border-radius: 8px;
            border: 2px solid %9;
            background-color: %6;
        }
        QRadioButton::indicator:hover {
            border: 2px solid %11;
        }
        QRadioButton::indicator:checked {
            border: 2px solid %11;
            background-color: qradialgradient(cx:0.5, cy:0.5, radius:0.4, fx:0.5, fy:0.5, stop:0 %11, stop:0.6 %11, stop:0.7 %6, stop:1 %6);
        }
        QRadioButton:disabled {
            color: #888888;
        }
        QRadioButton::indicator:disabled {
            border: 2px solid %9;
            background-color: %3;
        }
        QRadioButton::indicator:checked:disabled {
            border: 2px solid %9;
            background-color: qradialgradient(cx:0.5, cy:0.5, radius:0.4, fx:0.5, fy:0.5, stop:0 %9, stop:0.6 %9, stop:0.7 %3, stop:1 %3);
        }

        /* Group boxes */
        QGroupBox {
            background-color: %8;
            border: 1px solid %9;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 16px;
            font-weight: bold;
            color: %10;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0 6px;
            color: %10;
        }

        /* Combo boxes - Dropdown Style */
        QComboBox {
            background-color: %6;
            color: %7;
            border: 2px solid %9;
            border-radius: 5px;
            padding: 6px 30px 6px 10px;
            min-height: 32px;
            font-size: 13px;
        }
        QComboBox:hover {
            border: 2px solid %11;
            background-color: %5;
        }
        QComboBox:on {
            border: 2px solid %11;
            background-color: %6;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 26px;
            margin-right: 2px;
            border-left: 2px solid %9;
            border-top-right-radius: 4px;
            border-bottom-right-radius: 4px;
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %3, stop:1 %5);
        }
        QComboBox::drop-down:hover {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %11, stop:1 %12);
        }
        QComboBox::drop-down:pressed {
            background-color: %11;
        }
        QComboBox QAbstractItemView {
            background-color: %6;
            color: %7;
            selection-background-color: %12;
            selection-color: %13;
            border: 2px solid %11;
            border-radius: 4px;
            padding: 4px 8px;
            outline: 0px;
        }
        QComboBox QAbstractItemView::item {
            min-height: 28px;
            padding: 6px 10px;
            border-radius: 3px;
        }
        QComboBox QAbstractItemView::item:selected {
            background-color: %12;
            color: %13;
        }
        QComboBox QAbstractItemView::item:hover {
            background-color: %11;
            color: %13;
        }

        /* Spin boxes */
        QSpinBox {
            background-color: %6;
            color: %7;
            border: 1px solid %9;
            border-radius: 4px;
            padding: 4px;
            min-height: 24px;
        }
        QSpinBox:hover {
            border: 2px solid %11;
        }
        QSpinBox::up-button, QSpinBox::down-button {
            background-color: %3;
            border: 1px solid %9;
            border-radius: 2px;
            width: 16px;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover {
            background-color: %5;
        }

        /* Labels */
        QLabel {
            background-color: transparent;
            color: %2;
        }

        /* Progress bars */
        QProgressBar {
            background-color: %14;
            border: 1px solid %9;
            border-radius: 4px;
            text-align: center;
            height: 20px;
        }
        QProgressBar::chunk {
            background-color: %15;
            border-radius: 3px;
        }

        /* Scroll bars */
        QScrollBar:vertical {
            background-color: %1;
            width: 12px;
            border-radius: 6px;
        }
        QScrollBar::handle:vertical {
            background-color: %9;
            border-radius: 6px;
            min-height: 20px;
        }
        QScrollBar::handle:vertical:hover {
            background-color: %5;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar:horizontal {
            background-color: %1;
            height: 12px;
            border-radius: 6px;
        }
        QScrollBar::handle:horizontal {
            background-color: %9;
            border-radius: 6px;
            min-width: 20px;
        }
        QScrollBar::handle:horizontal:hover {
            background-color: %5;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }

        /* Tool tips */
        QToolTip {
            background-color: %1;
            color: %2;
            border: 1px solid %9;
            border-radius: 4px;
            padding: 4px;
        }

        /* Tab Widget */
        QTabWidget::pane {
            border: 1px solid %9;
            background-color: %1;
            border-radius: 4px;
        }
        QTabBar::tab {
            background-color: %3;
            color: %4;
            border: 1px solid %9;
            border-bottom: none;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            padding: 6px 16px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background-color: %1;
            color: %2;
            border-top: 3px solid %11;
        }
        QTabBar::tab:hover:!selected {
            background-color: %5;
        }

        /* ===== END UNIVERSAL STYLES ===== */
    )QSS")
    .arg(c.windowBackground)           // %1
    .arg(c.windowText)                 // %2
    .arg(c.buttonBackground)           // %3
    .arg(c.buttonText)                 // %4
    .arg(c.buttonHoverBackground)      // %5
    .arg(c.baseBackground)             // %6
    .arg(c.baseText)                   // %7
    .arg(c.groupBoxBackground)         // %8
    .arg(c.groupBoxBorder)             // %9
    .arg(c.groupBoxTitle)              // %10
    .arg(c.highlight)                  // %11
    .arg(c.selectionBackground)        // %12
    .arg(c.selectionText)              // %13
    .arg(c.progressBarBackground)      // %14
    .arg(c.progressBarChunk);          // %15

    return qss;
}

bool isSystemDarkMode() {
#ifdef _WIN32
    // Windows 10+ registry check for dark mode
    HKEY hKey;
    LONG result = RegOpenKeyExA(HKEY_CURRENT_USER, 
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 
        0, KEY_READ, &hKey);
    
    if (result == ERROR_SUCCESS) {
        DWORD value = 1; // Default to light
        DWORD size = sizeof(value);
        result = RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, nullptr, 
            reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(hKey);
        
        if (result == ERROR_SUCCESS) {
            return value == 0; // 0 = dark, 1 = light
        }
    }
    return false; // Default to light if detection fails
#else
    // For Linux/macOS, check environment variables
    const char* desktopEnv = getenv("XDG_CURRENT_DESKTOP");
    if (desktopEnv) {
        std::string de(desktopEnv);
        if (de.find("GNOME") != std::string::npos || de.find("KDE") != std::string::npos) {
            // Could check gsettings or kreadconfig5 here
            return false; // Default to light for now
        }
    }
    return false;
#endif
}

} // namespace aa
