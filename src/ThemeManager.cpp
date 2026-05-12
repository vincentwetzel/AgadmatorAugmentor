// Extracted from cpp directory
#include "ThemeManager.h"

#ifdef _WIN32
#include <windows.h>
#include <winreg.h>
#endif

namespace cta {

const char* ThemeManager::SETTINGS_ORG = "ChessTubeAnalyzer";
const char* ThemeManager::SETTINGS_APP = "settings";
const char* ThemeManager::SETTINGS_THEME_KEY = "themeMode";

ThemeManager::ThemeManager() : currentTheme_(ThemeMode::System), settings_(nullptr) {
    settings_ = new QSettings();
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
        c.controlBackground = "#2b2f33";
        c.controlHoverBackground = "#343a40";
        c.controlPressedBackground = "#24384a";
        c.controlBorder = "#515861";
        c.controlFocusBorder = "#4aa3ff";
        c.controlMutedText = "#a8b0ba";
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
        c.controlBackground = "#f7f9fc";
        c.controlHoverBackground = "#eef4fb";
        c.controlPressedBackground = "#dcecff";
        c.controlBorder = "#b9c4d0";
        c.controlFocusBorder = "#0078d4";
        c.controlMutedText = "#5f6b78";
        c.toggleCheckedBackground = "#4CAF50";
        c.toggleUncheckedBackground = "#888888";
        c.toggleThumb = "#ffffff";
    }
    
    return c;
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

} // namespace cta
