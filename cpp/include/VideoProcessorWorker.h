#pragma once

#include <QObject>
#include <QString>

namespace aa {

class VideoProcessorWorker : public QObject {
    Q_OBJECT

public:
    explicit VideoProcessorWorker(QObject* parent = nullptr);

public slots:
    void process(const QString& videoPath, const QString& boardAssetPath, const QString& outputPath);

signals:
    void progressUpdated(int percentage);
    void logMessage(const QString& message);
    void finished();
    void error(const QString& errorMessage);
};

} // namespace aa