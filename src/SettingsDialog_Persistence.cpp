#include "SettingsDialog.h"
#include "SysUtils.h"
#include "ToggleSwitch.h"
#include "ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <algorithm>

namespace cta {

void setThreadComboValue(QComboBox* comboBox, int threads) {
    if (!comboBox) {
        return;
    }

    const int clampedThreads = std::clamp(threads, 1, cta::SysUtils::max_hardware_thread_count());
    const int index = comboBox->findData(clampedThreads);
    comboBox->setCurrentIndex(index >= 0 ? index : comboBox->count() - 1);
}

QString canonicalVideoCodec(const QString& savedValue) {
    if (savedValue.contains("h264_nvenc") || savedValue.contains("NVIDIA GPU H.264")) return "h264_nvenc";
    if (savedValue.contains("libx265") || savedValue.contains("HEVC CPU")) return "libx265";
    if (savedValue.contains("hevc_nvenc") || savedValue.contains("NVIDIA GPU HEVC")) return "hevc_nvenc";
    if (savedValue.contains("libvpx-vp9") || savedValue.contains("VP9")) return "libvpx-vp9";
    return "libx264";
}

QString canonicalAudioCodec(const QString& savedValue) {
    if (savedValue.contains("libopus")) return "libopus";
    if (savedValue.contains("aac", Qt::CaseInsensitive) || savedValue.contains("AAC")) return "aac";
    return "copy";
}

QString canonicalResolution(const QString& savedValue) {
    if (savedValue.contains("3840") || savedValue.contains("4K")) return "4K";
    if (savedValue.contains("1920") || savedValue.contains("1080")) return "1080p";
    if (savedValue.contains("1280") || savedValue.contains("720")) return "720p";
    return "Source Resolution";
}



void SettingsDialog::loadSettings() {
    QSettings settings;
    const auto widgets = this->findChildren<QWidget*>();
    for (auto* w : widgets) w->blockSignals(true);

    const bool generatePgn = settings.value("generatePgn", true).toBool();
    ui.pgnExportToggle->setChecked(generatePgn);
    const bool includePgnMoveAnnotations = settings.value("pgnAnnotationsToggle", false).toBool();
    const bool includePgnAnalysis = settings.value("analysis/includePgnAnalysis", false).toBool() || includePgnMoveAnnotations;
    ui.pgnAnalysisToggle->setChecked(generatePgn && includePgnAnalysis);
    ui.pgnAnnotationsToggle->setChecked(generatePgn && includePgnMoveAnnotations && includePgnAnalysis);
    const bool generateSubtitles = settings.value("generateSubtitles", false).toBool();
    const bool generateAnalysisVideo = settings.value("generateAnalysisVideo", false).toBool() || generateSubtitles;
    ui.subtitlesToggle->setChecked(generateSubtitles);
    ui.analysisVideoToggle->setChecked(generateAnalysisVideo);
    ui.videoAnnotationsToggle->setChecked(generateAnalysisVideo && settings.value("analysis/enableMoveAnnotations", false).toBool());
    ui.pgnAnalysisToggle->setEnabled(generatePgn);
    ui.pgnAnnotationsToggle->setEnabled(generatePgn && includePgnAnalysis);
    ui.videoAnnotationsToggle->setEnabled(generateAnalysisVideo);

    int multiPv = settings.value("multiPv", 3).toInt();
    int multiPvIdx = ui.multiPvComboBox->findData(multiPv);
    ui.multiPvComboBox->setCurrentIndex(multiPvIdx >= 0 ? multiPvIdx : 2);
    const int defaultThreads = cta::SysUtils::max_hardware_thread_count();
    setThreadComboValue(ui.threadComboBox, settings.value("ffmpegThreads", defaultThreads).toInt());
    
    { int idx = ui.depthComboBox->findData(settings.value("stockfishDepth", 15).toInt()); ui.depthComboBox->setCurrentIndex(idx >= 0 ? idx : ui.depthComboBox->findData(15)); }
    { int idx = ui.timeComboBox->findData(settings.value("stockfishTime", 0).toInt()); ui.timeComboBox->setCurrentIndex(idx >= 0 ? idx : ui.timeComboBox->findData(0)); }
    { int idx = ui.nodesComboBox->findData(settings.value("stockfishNodes", 0).toInt()); ui.nodesComboBox->setCurrentIndex(idx >= 0 ? idx : ui.nodesComboBox->findData(0)); }
    { int idx = ui.analysisDepthComboBox->findData(settings.value("stockfishAnalysisDepth", 5).toInt()); ui.analysisDepthComboBox->setCurrentIndex(idx >= 0 ? idx : ui.analysisDepthComboBox->findData(5)); }
    ui.stockfishPathEdit->setText(settings.value("stockfishPath", "").toString());
    
    bool fpEnabled = settings.value("fastPreview", false).toBool();
    ui.fastPreviewToggle->setChecked(fpEnabled);
    ui.depthComboBox->setEnabled(!fpEnabled);
    ui.timeComboBox->setEnabled(!fpEnabled);
    ui.nodesComboBox->setEnabled(!fpEnabled);

    ui.debugLevelComboBox->setCurrentIndex(settings.value("debugLevel", 0).toInt());
    ui.themeComboBox->setCurrentIndex(settings.value("themeMode", 0).toInt());

    bool sameAsSource = settings.value("outSameAsSource", true).toBool();
    ui.sameAsSourceRadio->setChecked(sameAsSource);
    ui.customDirRadio->setChecked(!sameAsSource);
    ui.customDirEdit->setText(settings.value("outCustomDir", "").toString());
    ui.defaultVideoDirEdit->setText(settings.value("defaultVideoDir", "").toString());

    const QString videoCodec = canonicalVideoCodec(settings.value("videoCodec", "libx264").toString());
    int vIdx = ui.videoCodecComboBox->findData(videoCodec);
    ui.videoCodecComboBox->setCurrentIndex(vIdx >= 0 ? vIdx : 0);
    
    // Ensure dependent comboboxes have the correct items for the loaded codec before setting their values
    ui.audioCodecComboBox->clear(); ui.extensionComboBox->clear();
    if (ui.videoCodecComboBox->currentData().toString() == "libvpx-vp9") {
        ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
        ui.audioCodecComboBox->addItem("Opus audio (recommended for WebM)", "libopus");
        ui.extensionComboBox->addItems({".webm", ".mkv"});
    } else {
        ui.audioCodecComboBox->addItem("Keep original audio (fastest)", "copy");
        ui.audioCodecComboBox->addItem("AAC audio (standard)", "aac");
        ui.extensionComboBox->addItems({".mp4", ".mkv", ".avi", ".mov"});
    }
    
    const QString audioCodec = canonicalAudioCodec(settings.value("audioCodec", "copy").toString());
    int aIdx = ui.audioCodecComboBox->findData(audioCodec);
    ui.audioCodecComboBox->setCurrentIndex(aIdx >= 0 ? aIdx : 0);
    
    int eIdx = ui.extensionComboBox->findText(settings.value("videoExtension", ".mp4").toString());
    ui.extensionComboBox->setCurrentIndex(eIdx >= 0 ? eIdx : 0);
    
    const QString resolution = canonicalResolution(settings.value("videoResolution", "Source Resolution").toString());
    int rIdx = ui.resolutionComboBox->findData(resolution);
    ui.resolutionComboBox->setCurrentIndex(rIdx >= 0 ? rIdx : 0);
    
    int qval = settings.value("videoQuality", 23).toInt();
    int qidx = ui.qualityComboBox->findData(qval);
    ui.qualityComboBox->setCurrentIndex(qidx >= 0 ? qidx : 2);

    { int idx = ui.memoryLimitComboBox->findData(settings.value("memoryLimitMB", 0).toInt()); ui.memoryLimitComboBox->setCurrentIndex(idx >= 0 ? idx : ui.memoryLimitComboBox->findData(0)); }

    ui.logRetentionSpinBox->setValue(settings.value("logRetentionCount", 10).toInt());
    ui.compressLogsCheck->setChecked(settings.value("compressOldLogs", false).toBool());
    ui.removeOriginalToggle->setChecked(settings.value("removeOriginalVideo", false).toBool());

    for (auto* w : widgets) w->blockSignals(false);
}

void SettingsDialog::saveSettings() {
    QSettings settings;
    settings.setValue("generatePgn", ui.pgnExportToggle->isChecked());
    settings.setValue("analysis/includePgnAnalysis", ui.pgnAnalysisToggle->isChecked());
    settings.setValue("pgnAnnotationsToggle", ui.pgnAnnotationsToggle->isChecked());
    settings.setValue("generateSubtitles", ui.subtitlesToggle->isChecked());
    settings.setValue("generateAnalysisVideo", ui.analysisVideoToggle->isChecked());
    settings.setValue("multiPv", ui.multiPvComboBox->currentData().toInt());
    settings.setValue("ffmpegThreads", ui.threadComboBox->currentData().toInt());
    settings.setValue("themeMode", ui.themeComboBox->currentIndex());
    settings.setValue("analysis/enableMoveAnnotations", ui.videoAnnotationsToggle->isChecked());
    settings.setValue("removeOriginalVideo", ui.removeOriginalToggle->isChecked());

    settings.setValue("stockfishDepth", ui.depthComboBox->currentData().toInt());
    settings.setValue("stockfishTime", ui.timeComboBox->currentData().toInt());
    settings.setValue("stockfishNodes", ui.nodesComboBox->currentData().toInt());
    settings.setValue("stockfishAnalysisDepth", ui.analysisDepthComboBox->currentData().toInt());
    settings.setValue("stockfishPath", ui.stockfishPathEdit->text());
    settings.setValue("debugLevel", ui.debugLevelComboBox->currentIndex());

    settings.setValue("fastPreview", ui.fastPreviewToggle->isChecked());

    settings.setValue("outSameAsSource", ui.sameAsSourceRadio->isChecked());
    settings.setValue("outCustomDir", ui.customDirEdit->text());
    settings.setValue("defaultVideoDir", ui.defaultVideoDirEdit->text());
    settings.setValue("videoCodec", ui.videoCodecComboBox->currentData().toString());
    settings.setValue("audioCodec", ui.audioCodecComboBox->currentData().toString());
    settings.setValue("videoExtension", ui.extensionComboBox->currentText());
    settings.setValue("videoResolution", ui.resolutionComboBox->currentData().toString());
    settings.setValue("videoQuality", ui.qualityComboBox->currentData().toInt());
    
    settings.setValue("memoryLimitMB", ui.memoryLimitComboBox->currentData().toInt());
    settings.setValue("logRetentionCount", ui.logRetentionSpinBox->value());
    settings.setValue("compressOldLogs", ui.compressLogsCheck->isChecked());
}

void SettingsDialog::populateSettings(ProcessingSettings& s) const {
    const bool enableMoveAnnotations = ui.videoAnnotationsToggle->isChecked();

    s.generatePgn = ui.pgnExportToggle->isChecked();
    s.includePgnMoveAnnotations = ui.pgnExportToggle->isChecked() && ui.pgnAnalysisToggle->isChecked() && ui.pgnAnnotationsToggle->isChecked();
    s.includePgnAnalysis = ui.pgnExportToggle->isChecked() && (ui.pgnAnalysisToggle->isChecked() || s.includePgnMoveAnnotations);
    s.generateSubtitles = ui.subtitlesToggle->isChecked();
    s.enableMoveAnnotations = ui.analysisVideoToggle->isChecked() && enableMoveAnnotations;
    s.enableStockfish = s.includePgnAnalysis || s.includePgnMoveAnnotations || enableMoveAnnotations;
    s.generateAnalysisVideo = ui.analysisVideoToggle->isChecked() || ui.subtitlesToggle->isChecked();
    s.multiPv = ui.multiPvComboBox->currentData().toInt();
    s.ffmpegThreads = ui.threadComboBox->currentData().toInt();
    s.stockfishDepth = ui.depthComboBox->currentData().toInt();
    s.stockfishTime = ui.timeComboBox->currentData().toInt();
    s.stockfishNodes = ui.nodesComboBox->currentData().toInt();
    s.stockfishAnalysisDepth = ui.analysisDepthComboBox->currentData().toInt();
    s.stockfishPath = ui.stockfishPathEdit->text();
    s.debugLevel = ui.debugLevelComboBox->currentIndex();
    s.memoryLimitMB = ui.memoryLimitComboBox->currentData().toInt();

    if (ui.fastPreviewToggle->isChecked()) {
        s.stockfishDepth = 10;
        s.stockfishTime = 2; // 2 seconds max
        s.stockfishNodes = 0;
    }
}

void SettingsDialog::applySettingsToUi(const ProcessingSettings& settings) {
    ui.pgnExportToggle->setChecked(settings.generatePgn);
    ui.pgnAnalysisToggle->setChecked(settings.includePgnAnalysis);
    ui.pgnAnnotationsToggle->setChecked(settings.includePgnMoveAnnotations);
    ui.pgnAnalysisToggle->setEnabled(settings.generatePgn);
    ui.pgnAnnotationsToggle->setEnabled(settings.generatePgn && settings.includePgnAnalysis);
    ui.subtitlesToggle->setChecked(settings.generateSubtitles);
    ui.analysisVideoToggle->setChecked(settings.generateAnalysisVideo);
    ui.videoAnnotationsToggle->setChecked(settings.generateAnalysisVideo && settings.enableMoveAnnotations);
    ui.videoAnnotationsToggle->setEnabled(settings.generateAnalysisVideo);
    int idx = ui.multiPvComboBox->findData(settings.multiPv);
    if (idx >= 0) ui.multiPvComboBox->setCurrentIndex(idx);
    setThreadComboValue(ui.threadComboBox, settings.ffmpegThreads);
    { int idx = ui.depthComboBox->findData(settings.stockfishDepth); if (idx >= 0) ui.depthComboBox->setCurrentIndex(idx); }
    { int idx = ui.timeComboBox->findData(settings.stockfishTime); if (idx >= 0) ui.timeComboBox->setCurrentIndex(idx); }
    { int idx = ui.nodesComboBox->findData(settings.stockfishNodes); if (idx >= 0) ui.nodesComboBox->setCurrentIndex(idx); }
    { int idx = ui.analysisDepthComboBox->findData(settings.stockfishAnalysisDepth); if (idx >= 0) ui.analysisDepthComboBox->setCurrentIndex(idx); }
    ui.stockfishPathEdit->setText(settings.stockfishPath);
    ui.debugLevelComboBox->setCurrentIndex(settings.debugLevel);
}

void SettingsDialog::applyHeadlessOverrides(int pgnOverride, int analysisVideoOverride, int moveLabelsOverride, int multiPv, int threads, int depth, int time, int nodes, int analysisDepth, const QString& debugLevelStr, int memoryLimit) {
    if (pgnOverride != -1) ui.pgnExportToggle->setChecked(pgnOverride != 0);
    if (analysisVideoOverride != -1) ui.analysisVideoToggle->setChecked(analysisVideoOverride != 0);
    if (moveLabelsOverride != -1) ui.videoAnnotationsToggle->setChecked(moveLabelsOverride != 0);
    if (multiPv > 0) {
        int idx = ui.multiPvComboBox->findData(multiPv);
        if (idx >= 0) ui.multiPvComboBox->setCurrentIndex(idx);
    }
    if (threads > 0) setThreadComboValue(ui.threadComboBox, threads);
    if (depth > 0) { int idx = ui.depthComboBox->findData(depth); if (idx >= 0) ui.depthComboBox->setCurrentIndex(idx); }
    if (time >= 0) { int idx = ui.timeComboBox->findData(time); if (idx >= 0) ui.timeComboBox->setCurrentIndex(idx); }
    if (nodes >= 0) { int idx = ui.nodesComboBox->findData(nodes); if (idx >= 0) ui.nodesComboBox->setCurrentIndex(idx); }
    if (analysisDepth > 0) { int idx = ui.analysisDepthComboBox->findData(analysisDepth); if (idx >= 0) ui.analysisDepthComboBox->setCurrentIndex(idx); }
    if (memoryLimit >= 0) { int idx = ui.memoryLimitComboBox->findData(memoryLimit); if (idx >= 0) ui.memoryLimitComboBox->setCurrentIndex(idx); }
    if (!debugLevelStr.isEmpty()) {
        if (debugLevelStr == "NONE") ui.debugLevelComboBox->setCurrentIndex(0);
        else if (debugLevelStr == "MOVES") ui.debugLevelComboBox->setCurrentIndex(1);
        else if (debugLevelStr == "FULL") ui.debugLevelComboBox->setCurrentIndex(2);
    }
}


} // namespace cta
