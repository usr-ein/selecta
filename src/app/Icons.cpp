// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Icons.h"

#include <QPainter>

namespace selecta::app::icons {

QIcon symbol(const QString &sfSymbol, const QString &fallbackThemeName)
{
    QIcon icon = QIcon::fromTheme(sfSymbol);
    if (icon.isNull() && !fallbackThemeName.isEmpty()) {
        icon = QIcon::fromTheme(fallbackThemeName);
    }
    return icon;
}

QPixmap tinted(const QIcon &icon, QSize size, qreal devicePixelRatio, const QColor &color)
{
    if (icon.isNull() || size.isEmpty()) {
        return {};
    }
    const QPixmap source = icon.pixmap(size, devicePixelRatio);
    if (source.isNull()) {
        return {};
    }
    QPixmap result(source.size());
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRectF(QPointF(0, 0), result.deviceIndependentSize()), color);
    return result;
}

} // namespace selecta::app::icons
