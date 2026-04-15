// Extracted from cpp directory
#include "ToggleSwitch.h"
#include "ThemeManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QLabel>

ToggleSwitch::ToggleSwitch(const QString& label, bool checked, QWidget* parent)
    : QWidget(parent), checked_(checked), label_(label) {
    setFixedSize(60, 30);
    setCursor(Qt::PointingHandCursor);
    setToolTip("Click to toggle");
}

bool ToggleSwitch::isChecked() const {
    return checked_;
}

void ToggleSwitch::setChecked(bool checked) {
    if (checked_ != checked) {
        checked_ = checked;
        emit toggled(checked_);
        update();
    }
}

QString ToggleSwitch::label() const {
    return label_;
}

void ToggleSwitch::setLabel(const QString& label) {
    label_ = label;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Get theme colors from ThemeManager
    auto colors = aa::ThemeManager::instance().colors();

    // Draw track background
    QRectF trackRect(2, 5, 56, 20);

    if (checked_) {
        // Enabled/Checked state - use theme color
        painter.setBrush(QColor(colors.toggleCheckedBackground));
    } else {
        // Disabled/Unchecked state - use theme color
        painter.setBrush(QColor(colors.toggleUncheckedBackground));
    }

    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(trackRect, 10, 10);

    // Draw thumb (the circle)
    QRectF thumbRect;
    qreal thumbRadius = 8;

    if (checked_) {
        thumbRect = QRectF(42 - thumbRadius, 15 - thumbRadius, thumbRadius * 2, thumbRadius * 2);
    } else {
        thumbRect = QRectF(12 - thumbRadius, 15 - thumbRadius, thumbRadius * 2, thumbRadius * 2);
    }

    painter.setBrush(QColor(colors.toggleThumb));
    painter.drawEllipse(thumbRect);
}

void ToggleSwitch::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        checked_ = !checked_;
        emit toggled(checked_);
        update();
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}
