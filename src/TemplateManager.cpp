#include "TemplateManager.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace cta {

TemplateManager& TemplateManager::instance() {
    static TemplateManager instance;
    return instance;
}

namespace {
std::string normalizeArrowsTarget(const std::string& arrowsTarget) {
    if (arrowsTarget == "Debug Board" || arrowsTarget == "Board Overlay") {
        return "Analysis Board";
    }
    return arrowsTarget;
}

OverlayTemplate parseTemplateJson(const nlohmann::json& j, const QString& defaultId) {
    OverlayTemplate tpl;
    tpl.id = QString::fromStdString(j.value("id", defaultId.toStdString()));
    tpl.name = QString::fromStdString(j.value("name", "Unknown Template"));
    tpl.screenshotFilename = QString::fromStdString(j.value("screenshotFilename", ""));
    tpl.isBuiltIn = j.value("isBuiltIn", false);
    
    if (j.contains("keywords") && j["keywords"].is_array()) {
        for (const auto& kw : j["keywords"]) {
            tpl.keywords.append(QString::fromStdString(kw.get<std::string>()));
        }
    }

    if (j.contains("config")) {
        auto& cfg = j["config"];
        tpl.config.board.enabled = cfg.value("boardEnabled", true);
        tpl.config.board.x_percent = cfg.value("boardX", 1.0);
        tpl.config.board.y_percent = cfg.value("boardY", 0.0);
        tpl.config.board.scale = cfg.value("boardScale", 0.3);
        
        tpl.config.evalBar.enabled = cfg.value("evalBarEnabled", true);
        tpl.config.evalBar.x_percent = cfg.value("evalBarX", 0.0);
        tpl.config.evalBar.y_percent = cfg.value("evalBarY", 0.0);
        tpl.config.evalBar.scale = cfg.value("evalBarScale", 1.0);
        
        tpl.config.pvText.enabled = cfg.value("pvTextEnabled", true);
        tpl.config.pvText.x_percent = cfg.value("pvTextX", 0.5);
        tpl.config.pvText.y_percent = cfg.value("pvTextY", 0.95);
        tpl.config.pvText.scale = cfg.value("pvTextScale", 1.0);

        tpl.config.arrowsTarget = normalizeArrowsTarget(cfg.value("arrowsTarget", "Analysis Board"));
    }
    return tpl;
}
} // namespace

void TemplateManager::initialize() {
    // Define the AppData templates directory: %APPDATA%/ChessTubeAnalyzer/templates
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir appDataDir(appDataPath);
    if (!appDataDir.exists()) {
        appDataDir.mkpath(".");
    }
    
    appDataTemplateDir_ = appDataDir.absoluteFilePath("templates");
    QDir templateDir(appDataTemplateDir_);
    if (!templateDir.exists()) {
        templateDir.mkpath(".");
    }

    // Source directory (Option B: alongside the executable)
    QString bundledTemplateDir = QCoreApplication::applicationDirPath() + "/templates";
    QDir bundledDir(bundledTemplateDir);
    
    // Fallback for dev environment (e.g., running from build/Release)
    if (!bundledDir.exists()) {
        bundledTemplateDir = QCoreApplication::applicationDirPath() + "/../../templates";
        bundledDir.setPath(bundledTemplateDir);
    }

    // Copy bundled templates to AppData if they don't already exist there
    if (bundledDir.exists()) {
        QStringList filters = {"*.json", "*.png", "*.jpg"};
        QFileInfoList fileList = bundledDir.entryInfoList(filters, QDir::Files);
        for (const QFileInfo& fileInfo : fileList) {
            QString destFilePath = templateDir.absoluteFilePath(fileInfo.fileName());
            if (!QFile::exists(destFilePath)) {
                QFile::copy(fileInfo.absoluteFilePath(), destFilePath);
                
                // Make sure the copied file is writable in AppData
                QFile destFile(destFilePath);
                destFile.setPermissions(destFile.permissions() | QFileDevice::WriteUser);
            }
        }
    }

    reloadTemplates();
}

void TemplateManager::reloadTemplates() {
    QDir templateDir(appDataTemplateDir_);
    templates_.clear();
    QFileInfoList jsonFiles = templateDir.entryInfoList({"*.json"}, QDir::Files);
    
    for (const QFileInfo& fileInfo : jsonFiles) {
        QFile file(fileInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        try {
            QByteArray data = file.readAll();
            nlohmann::json j = nlohmann::json::parse(data.toStdString());
            file.close();
            templates_.push_back(parseTemplateJson(j, fileInfo.baseName()));
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse template JSON: " << fileInfo.fileName().toStdString() << " - " << e.what() << "\n";
        }
    }
}

std::vector<OverlayTemplate> TemplateManager::getAllTemplates() const {
    return templates_;
}

std::optional<OverlayTemplate> TemplateManager::getTemplate(const QString& id) const {
    for (const auto& tpl : templates_) {
        if (tpl.id == id) return tpl;
    }
    return std::nullopt;
}

OverlayTemplate TemplateManager::getFallbackTemplate() const {
    auto genericTpl = getTemplate("generic");
    if (genericTpl.has_value()) {
        return genericTpl.value();
    }
    // Ultimate hardcoded fallback if even "generic.json" is missing
    OverlayTemplate safe;
    safe.id = "generic";
    safe.name = "Generic Default";
    safe.config.board.enabled = true;
    safe.config.board.x_percent = 1.0;
    safe.config.board.y_percent = 0.0;
    safe.config.board.scale = 0.3;
    safe.config.arrowsTarget = "Analysis Board";
    return safe;
}

OverlayTemplate TemplateManager::matchTemplate(const QString& videoFilename) const {
    // Case-insensitive match against keywords
    QString lowerFilename = videoFilename.toLower();
    for (const auto& tpl : templates_) {
        // Primary check: Does the filename contain the template's exact name or ID?
        if (!tpl.name.trimmed().isEmpty() && lowerFilename.contains(tpl.name.trimmed().toLower())) {
            return tpl;
        }
        if (!tpl.id.trimmed().isEmpty() && lowerFilename.contains(tpl.id.trimmed().toLower())) {
            return tpl;
        }
        
        // Fallback check: Check custom user-provided keywords/abbreviations
        for (const QString& kw : tpl.keywords) {
            if (!kw.trimmed().isEmpty() && lowerFilename.contains(kw.trimmed().toLower())) {
                return tpl;
            }
        }
    }
    return getFallbackTemplate();
}

bool TemplateManager::loadTemplate(const QString& id, QString* errorMessage) {
    QString filePath = QDir(appDataTemplateDir_).absoluteFilePath(id + ".json");
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) *errorMessage = "Could not open template file: " + filePath;
        return false;
    }

    try {
        QByteArray data = file.readAll();
        nlohmann::json j = nlohmann::json::parse(data.toStdString());
        file.close();

        OverlayTemplate tpl = parseTemplateJson(j, id);
        auto it = std::find_if(templates_.begin(), templates_.end(), [&](const OverlayTemplate& t) { return t.id == tpl.id; });
        if (it != templates_.end()) *it = tpl;
        else templates_.push_back(tpl);

        return true;
    } catch (const std::exception& e) {
        if (errorMessage) *errorMessage = QString("Failed to parse template JSON: ") + e.what();
        return false;
    }
}

QString TemplateManager::getScreenshotPath(const QString& screenshotFilename) const {
    return QDir(appDataTemplateDir_).absoluteFilePath(screenshotFilename);
}

bool TemplateManager::saveTemplate(const OverlayTemplate& tpl, QString* errorMessage) {
    if (errorMessage) {
        errorMessage->clear();
    }

    nlohmann::json j;
    j["id"] = tpl.id.toStdString();
    j["name"] = tpl.name.toStdString();
    j["screenshotFilename"] = tpl.screenshotFilename.toStdString();
    j["isBuiltIn"] = tpl.isBuiltIn;
    
    nlohmann::json kw_array = nlohmann::json::array();
    for (const QString& kw : tpl.keywords) {
        kw_array.push_back(kw.toStdString());
    }
    j["keywords"] = kw_array;

    nlohmann::json cfg;
    cfg["boardEnabled"] = tpl.config.board.enabled;
    cfg["boardX"] = tpl.config.board.x_percent;
    cfg["boardY"] = tpl.config.board.y_percent;
    cfg["boardScale"] = tpl.config.board.scale;
    
    cfg["evalBarEnabled"] = tpl.config.evalBar.enabled;
    cfg["evalBarX"] = tpl.config.evalBar.x_percent;
    cfg["evalBarY"] = tpl.config.evalBar.y_percent;
    cfg["evalBarScale"] = tpl.config.evalBar.scale;
    
    cfg["pvTextEnabled"] = tpl.config.pvText.enabled;
    cfg["pvTextX"] = tpl.config.pvText.x_percent;
    cfg["pvTextY"] = tpl.config.pvText.y_percent;
    cfg["pvTextScale"] = tpl.config.pvText.scale;
    cfg["arrowsTarget"] = tpl.config.arrowsTarget;

    j["config"] = cfg;

    QString filePath = QDir(appDataTemplateDir_).absoluteFilePath(tpl.id + ".json");
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("Failed to open template file for writing: %1").arg(filePath);
        }
        return false;
    }
    std::string dumpStr = j.dump(4);
    f.write(dumpStr.data(), dumpStr.size());
    f.close();

    // Update in-memory vector
    auto it = std::find_if(templates_.begin(), templates_.end(), [&](const OverlayTemplate& t) { return t.id == tpl.id; });
    if (it != templates_.end()) *it = tpl;
    else templates_.push_back(tpl);
    
    return true;
}

bool TemplateManager::deleteTemplate(const QString& id, QString* errorMessage) {
    if (errorMessage) {
        errorMessage->clear();
    }

    auto it = std::find_if(templates_.begin(), templates_.end(), [&](const OverlayTemplate& t) { return t.id == id; });
    if (it == templates_.end()) {
        if (errorMessage) {
            *errorMessage = QString("Template not found: %1").arg(id);
        }
        return false;
    }
    if (it->isBuiltIn) {
        if (errorMessage) {
            *errorMessage = QString("Built-in template cannot be deleted: %1").arg(id);
        }
        return false;
    }

    const QString templatePath = QDir(appDataTemplateDir_).absoluteFilePath(id + ".json");
    if (QFile::exists(templatePath) && !QFile::remove(templatePath)) {
        if (errorMessage) {
            *errorMessage = QString("Failed to delete template file: %1").arg(templatePath);
        }
        return false;
    }

    if (!it->screenshotFilename.isEmpty()) {
        const QString screenshotPath = getScreenshotPath(it->screenshotFilename);
        if (QFile::exists(screenshotPath) && !QFile::remove(screenshotPath)) {
            if (errorMessage) {
                *errorMessage = QString("Failed to delete screenshot file: %1").arg(screenshotPath);
            }
            return false;
        }
    }

    templates_.erase(it);
    return true;
}

} // namespace cta
