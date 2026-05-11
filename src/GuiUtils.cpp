#include "GuiUtils.h"

#include <QRegularExpression>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace cta {
namespace gui {

QString formatElapsedPrefix(qint64 totalMs) {
    const qint64 h = totalMs / 3600000;
    const qint64 m = (totalMs % 3600000) / 60000;
    const qint64 s = (totalMs % 60000) / 1000;
    const qint64 ms = totalMs % 1000;

    if (h > 0) {
        return QString("[%1:%2:%3.%4]")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'))
            .arg(ms, 3, 10, QChar('0'));
    }

    return QString("[%1:%2.%3]")
        .arg(m)
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

bool hasElapsedPrefix(const QString& line) {
    static const QRegularExpression elapsedPrefixPattern(
        QStringLiteral("^\\s*\\[(?:\\d+:)?\\d+:\\d{2}\\.\\d{3}\\]"));
    return elapsedPrefixPattern.match(line).hasMatch();
}

QIcon createSettingsCogIcon(const QColor& color) {
    constexpr int canvasSize = 96;
    constexpr qreal center = canvasSize / 2.0;
    constexpr qreal bodyRadius = 24.0;
    constexpr qreal ringCutoutRadius = 14.0;
    constexpr qreal hubRadius = 4.75;
    constexpr qreal toothCenterRadius = 28.5;
    constexpr qreal toothWidth = 8.5;
    constexpr qreal toothHeight = 15.0;
    constexpr qreal toothCornerRadius = 4.0;

    QPixmap pixmap(canvasSize, canvasSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    QPainterPath gearPath;
    gearPath.addEllipse(QPointF(center, center), bodyRadius, bodyRadius);

    for (int i = 0; i < 8; ++i) {
        painter.save();
        painter.translate(center, center);
        painter.rotate(i * 45.0);
        QPainterPath toothPath;
        toothPath.addRoundedRect(
            QRectF(-toothWidth / 2.0, -toothCenterRadius - toothHeight / 2.0, toothWidth, toothHeight),
            toothCornerRadius,
            toothCornerRadius
        );
        gearPath.addPath(painter.transform().map(toothPath));
        painter.restore();
    }

    QColor fillColor = color;
    fillColor.setAlpha(242);
    painter.setBrush(fillColor);
    painter.drawPath(gearPath.simplified());

    QColor rimColor = color.lighter(112);
    rimColor.setAlpha(80);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(rimColor, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QPointF(center, center), bodyRadius - 1.5, bodyRadius - 1.5);

    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::transparent);
    painter.drawEllipse(QPointF(center, center), ringCutoutRadius, ringCutoutRadius);
    painter.drawEllipse(QPointF(center, center), hubRadius, hubRadius);

    return QIcon(pixmap);
}

} // namespace gui
} // namespace cta