#include "ThemeManager.h"

namespace cta {

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
            background-color: %1;
            color: %2;
            border: 2px solid %9;
            border-radius: 6px;
            padding: 6px 10px;
            selection-background-color: %12;
            selection-color: %13;
        }
        QLineEdit:hover, QTextEdit:hover, QPlainTextEdit:hover {
            border: 2px solid %11;
        }
        QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {
            border: 2px solid %11;
            background-color: %1;
        }
        QLineEdit:disabled, QTextEdit:disabled, QPlainTextEdit:disabled {
            background-color: %3;
            color: #888888;
            border: 2px solid %9;
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

        /* Combo boxes */
        QComboBox {
            background-color: %16;
            color: %2;
            border: 1px solid %19;
            border-radius: 5px;
            padding: 6px 34px 6px 10px;
            min-height: 24px;
        }
        QComboBox:hover {
            background-color: %17;
            border: 1px solid %20;
        }
        QComboBox:focus, QComboBox:on {
            background-color: %16;
            border: 1px solid %20;
        }
        QComboBox::drop-down {
            subcontrol-origin: border;
            subcontrol-position: top right;
            width: 30px;
            border-left: 1px solid %19;
            border-top-right-radius: 5px;
            border-bottom-right-radius: 5px;
            background-color: transparent;
        }
        QComboBox::drop-down:hover {
            background-color: %17;
        }
        QComboBox::drop-down:pressed, QComboBox:on::drop-down {
            background-color: %18;
            border-left: 1px solid %20;
        }
        QComboBox::down-arrow {
            width: 9px;
            height: 9px;
        }
        QComboBox QAbstractItemView {
            background-color: %16;
            color: %2;
            selection-background-color: %12;
            selection-color: %13;
            border: 1px solid %20;
            border-radius: 6px;
            padding: 5px 0px;
            outline: 0px;
        }
        QComboBox QAbstractItemView::item {
            min-height: 24px;
            padding: 5px 10px;
            border-radius: 4px;
            margin: 1px 4px;
        }
        QComboBox QAbstractItemView::item:selected, QComboBox QAbstractItemView::item:hover {
            background-color: %12;
            color: %13;
        }
        QComboBox:disabled {
            background-color: %3;
            color: %21;
            border: 1px solid %9;
        }

        /* Spin boxes */
        QSpinBox {
            background-color: %16;
            color: %2;
            border: 1px solid %19;
            border-radius: 5px;
            padding: 6px 34px 6px 10px;
            min-height: 24px;
        }
        QSpinBox:hover {
            background-color: %17;
            border: 1px solid %20;
        }
        QSpinBox:focus {
            background-color: %16;
            border: 1px solid %20;
        }
        QSpinBox::up-button, QSpinBox::down-button {
            subcontrol-origin: border;
            width: 30px;
            background-color: transparent;
            border-left: 1px solid %19;
        }
        QSpinBox::up-button {
            subcontrol-position: top right;
            border-top-right-radius: 5px;
            border-bottom: 1px solid %19;
        }
        QSpinBox::down-button {
            subcontrol-position: bottom right;
            border-bottom-right-radius: 5px;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover {
            background-color: %17;
        }
        QSpinBox::up-button:pressed, QSpinBox::down-button:pressed {
            background-color: %18;
            border-left: 1px solid %20;
        }
        QSpinBox::up-arrow, QSpinBox::down-arrow {
            width: 8px;
            height: 8px;
        }
        QSpinBox:disabled {
            background-color: %3;
            color: %21;
            border: 1px solid %9;
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
        /* Safety net to prevent QString::arg() shifting bugs if a marker is removed:
           %1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11 %12 %13 %14 %15 %16 %17 %18 %19 %20 %21 */
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
    .arg(c.progressBarChunk)           // %15
    .arg(c.controlBackground)          // %16
    .arg(c.controlHoverBackground)     // %17
    .arg(c.controlPressedBackground)   // %18
    .arg(c.controlBorder)              // %19
    .arg(c.controlFocusBorder)         // %20
    .arg(c.controlMutedText);          // %21

    return qss;
}

} // namespace cta
