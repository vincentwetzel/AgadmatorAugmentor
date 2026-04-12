#include "MainWindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
    // Enable high DPI scaling for modern displays
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    
    QApplication app(argc, argv);
    QApplication::setApplicationName("Agadmator Augmentor GUI");
    QApplication::setApplicationVersion("0.2.0");

    aa::MainWindow window;
    window.show();

    return app.exec();
}