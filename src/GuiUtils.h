#pragma once

#include <QString>
#include <QIcon>
#include <QColor>
#include <QtGlobal>

namespace cta {
namespace gui {

// Format a millisecond duration into a logging prefix like [01:23.456]
QString formatElapsedPrefix(qint64 totalMs);
// Check if a log line already contains an elapsed time prefix
bool hasElapsedPrefix(const QString& line);
// Procedurally generate the settings cog icon to match theme colors
QIcon createSettingsCogIcon(const QColor& color);

} // namespace gui
} // namespace cta