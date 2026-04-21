#pragma once

#include <QString>
#include <QStringList>
#include <vector>
#include <optional>
#include "ProcessingSettings.h" // Assuming VideoOverlayConfig is defined here or in a similar header

namespace aa {

struct OverlayTemplate {
    QString id;                 // Unique identifier (e.g., "agadmator")
    QString name;               // Display name (e.g., "Agadmator's Chess Channel")
    QStringList keywords;       // Substrings/Regex to match against video filenames
    QString screenshotFilename; // e.g., "agadmator_ref.png"
    VideoOverlayConfig config;  // The actual layout settings
    bool isBuiltIn = false;     // True if it's a default app template (restricts deletion)
};

class TemplateManager {
public:
    static TemplateManager& instance();

    // Copies default templates from app directory to %APPDATA% if they don't exist, then loads them
    void initialize();

    // Getters
    std::vector<OverlayTemplate> getAllTemplates() const;
    std::optional<OverlayTemplate> getTemplate(const QString& id) const;
    
    // Scans a video filename against all template keywords and returns the best match (or Generic)
    OverlayTemplate matchTemplate(const QString& videoFilename) const;
    
    // The ultimate fallback template
    OverlayTemplate getFallbackTemplate() const;

    // Setters
    bool saveTemplate(const OverlayTemplate& tpl);
    bool deleteTemplate(const QString& id);

    // Helper to get the absolute path to a template's reference screenshot
    QString getScreenshotPath(const QString& screenshotFilename) const;

private:
    TemplateManager() = default;
    ~TemplateManager() = default;

    std::vector<OverlayTemplate> templates_;
    QString appDataTemplateDir_;
};

} // namespace aa