// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Appearance.h"

#include <QPalette>

#include <gtest/gtest.h>

using selecta::app::isDarkPalette;
using selecta::app::symbolAccentColor;

namespace {

QPalette paletteWith(const QColor &window, const QColor &text, const QColor &accent)
{
    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Accent, accent);
    return palette;
}

const QColor kQtAccent(0x30, 0x8C, 0xC6);
QPalette lightPalette()
{
    return paletteWith(QColor(0xEC, 0xEC, 0xEC), Qt::black, kQtAccent);
}

QPalette darkPalette()
{
    return paletteWith(QColor(0x1E, 0x1E, 0x1E), Qt::white, kQtAccent);
}

} // namespace

TEST(AppearanceTest, TellsDarkPalettesFromLightOnes)
{
    EXPECT_FALSE(isDarkPalette(lightPalette()));
    EXPECT_TRUE(isDarkPalette(darkPalette()));
}

TEST(AppearanceTest, SymbolAccentIsLighterInDarkMode)
{
    const QColor light = symbolAccentColor(lightPalette());
    const QColor dark = symbolAccentColor(darkPalette());
    ASSERT_TRUE(light.isValid());
    ASSERT_TRUE(dark.isValid());
#ifdef Q_OS_MACOS
    // The system's accent, resolved in Aqua and Dark Aqua: never the palette's own colour, and lighter on dark
    // (the beta's sidebar icons were too dark a blue in dark mode).
    EXPECT_EQ(light, selecta::app::macSystemAccentColor(false));
    EXPECT_EQ(dark, selecta::app::macSystemAccentColor(true));
    EXPECT_GT(dark.lightness(), light.lightness()) << dark.name().toStdString() << " vs " << light.name().toStdString();
#else
    EXPECT_EQ(light, kQtAccent); // the platform theme's accent
    EXPECT_EQ(dark, kQtAccent);
#endif
}
