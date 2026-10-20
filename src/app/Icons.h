// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace selecta::app::icons {

// An SF Symbol on macOS (QIcon::fromTheme hands unknown names to NSImage), else the freedesktop fallback name.
// SF Symbols are never bundled (ADR-010). May return a null icon on platforms without either.
QIcon symbol(const QString &sfSymbol, const QString &fallbackThemeName = {});

// The icon rendered at `size` (device-independent pixels) and recoloured with `color`, keeping its alpha.
QPixmap tinted(const QIcon &icon, QSize size, qreal devicePixelRatio, const QColor &color);

// Selecta's own icon, the record, from 16 to 256 px (packaging/icons, compiled in). Linux and Windows show it on
// windows and in the task bar; macOS takes the app icon from the bundle instead. UI thread only.
QIcon applicationIcon();

} // namespace selecta::app::icons
