#pragma once

#include <QMainWindow>
#include <QThread>

class QLineEdit;
class QTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace aa {

class VideoProcessorWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void browseVideo();
    void startProcessing();
    void appendLog(const QString& message);
    void updateProgress(int percentage);
    void processingFinished();
    void processingError(const QString& errorMessage);

private:
    QLineEdit* videoPathEdit_;
    QPushButton* browseBtn_;
    QPushButton* startBtn_;
    QTextEdit* logOutput_;
    QProgressBar* progressBar_;
    QSpinBox* threadSpinBox_;

    QThread workerThread_;
    VideoProcessorWorker* worker_;
};

} // namespace aa