// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Appearance.h"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::app {

QString appearanceKey(Appearance appearance)
{
    switch (appearance) {
    case Appearance::System:
        return u"system"_s;
    case Appearance::Light:
        return u"light"_s;
    case Appearance::Dark:
        return u"dark"_s;
    }
    return u"system"_s;
}

Appearance appearanceFromKey(QStringView key, Appearance fallback)
{
    if (key == u"system") {
        return Appearance::System;
    }
    if (key == u"light") {
        return Appearance::Light;
    }
    if (key == u"dark") {
        return Appearance::Dark;
    }
    return fallback;
}

void applyAppearance(Appearance appearance)
{
    QStyleHints *hints = QGuiApplication::styleHints();
    switch (appearance) {
    case Appearance::System:
        hints->unsetColorScheme();
        break;
    case Appearance::Light:
        hints->setColorScheme(Qt::ColorScheme::Light);
        break;
    case Appearance::Dark:
        hints->setColorScheme(Qt::ColorScheme::Dark);
        break;
    }
}

bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness();
}

QColor qualityTierColor(int tier, const QPalette &palette)
{
    // As core::QualityTier counts: unknown, below 192, 192–255, 256–319, 320 or V0, from lossless.
    struct Pair
    {
        QColor light;
        QColor dark;
    };
    static const std::array<Pair, 6> kColours{{
        {QColor(0x8E, 0x8E, 0x93), QColor(0x98, 0x98, 0x9D)}, // grey
        {QColor(0xFF, 0x3B, 0x30), QColor(0xFF, 0x45, 0x3A)}, // red
        {QColor(0xFF, 0x95, 0x00), QColor(0xFF, 0x9F, 0x0A)}, // orange
        {QColor(0xFF, 0xCC, 0x00), QColor(0xFF, 0xD6, 0x0A)}, // yellow
        {QColor(0x34, 0xC7, 0x59), QColor(0x30, 0xD1, 0x58)}, // green
        {QColor(0x00, 0x7A, 0xFF), QColor(0x0A, 0x84, 0xFF)}, // blue
    }};
    const Pair &pair = kColours.at(tier >= 0 && tier < static_cast<int>(kColours.size()) ? tier : 0);
    return isDarkPalette(palette) ? pair.dark : pair.light;
}

QColor symbolAccentColor(const QPalette &palette)
{
#ifdef Q_OS_MACOS
    if (const QColor system = macSystemAccentColor(isDarkPalette(palette)); system.isValid()) {
        return system;
    }
#endif
    return palette.color(QPalette::Accent);
}

} // namespace selecta::app
