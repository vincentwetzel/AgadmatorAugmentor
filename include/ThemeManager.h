#pragma once

#include <QString>
#include <QSettings>

namespace cta {

enum class ThemeMode {
    System,
    Light,
    Dark
};

class ThemeManager {
public:
    struct ThemeColors {
        QString windowBackground;
        QString windowText;
        QString buttonBackground;
        QString buttonText;
        QString buttonHoverBackground;
        QString baseBackground;
        QString baseText;
        QString groupBoxBackground;
        QString groupBoxBorder;
        QString groupBoxTitle;
        QString highlight;
        QString highlightText;
        QString selectionBackground;
        QString selectionText;
        QString progressBarBackground;
        QString progressBarChunk;
        QString controlBackground;
        QString controlHoverBackground;
        QString controlPressedBackground;
        QString controlBorder;
        QString controlFocusBorder;
        QString controlMutedText;
        QString toggleCheckedBackground;
        QString toggleUncheckedBackground;
        QString toggleThumb;
    };

    static ThemeManager& instance();

    ThemeMode currentTheme() const { return currentTheme_; }
    void setTheme(ThemeMode mode);
    QString themeName() const;
    ThemeColors colors() const;
    QString generateStyleSheet() const;

private:
    ThemeManager();
    ~ThemeManager() = default;

    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    void loadSettings();
    void saveSettings() const;

    static const char* SETTINGS_ORG;
    static const char* SETTINGS_APP;
    static const char* SETTINGS_THEME_KEY;

    ThemeMode currentTheme_;
    QSettings* settings_;
};

bool isSystemDarkMode();

} // namespace cta
