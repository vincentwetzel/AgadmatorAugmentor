#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include "ProcessingSettings.h"

namespace aa {

class VideoProcessorWorker : public QObject {
    Q_OBJECT

public:
    explicit VideoProcessorWorker(QObject* parent = nullptr) : QObject(parent) {}

public slots:
    void process(const ProcessingSettings& settings, std::atomic<bool>* cancelFlag);

signals:
    void progressUpdated(int percentage);
    void logMessage(const QString& message);
    void finished();
    void error(const QString& errorMessage);
};

} // namespace aa