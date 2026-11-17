// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QString>
#include <QStringView>

class QPalette;

namespace selecta::app {

// Light / dark appearance (PRD UI-2). System follows macOS.
enum class Appearance { System, Light, Dark };

QString appearanceKey(Appearance appearance); // "system", "light", "dark"
Appearance appearanceFromKey(QStringView key, Appearance fallback = Appearance::System);

// Applies the appearance to the whole application. Main thread only.
void applyAppearance(Appearance appearance);

// Whether the palette is a dark one (its window darker than its text).
bool isDarkPalette(const QPalette &palette);

// The accent colour for tinted symbols (the sidebar's icons), in the palette's appearance. On macOS it is the
// system's accent (NSColor.controlAccentColor), so it follows the user's choice in System Settings; on a dark palette
// it is that accent lifted a step toward the system highlight, so thin symbols stay readable on a dark sidebar.
// Elsewhere it is the palette's Accent. Main thread only.
QColor symbolAccentColor(const QPalette &palette);

// The colour of a quality tier's dot on the cover wall and the album page (P-010; core::QualityTier, as an int): the
// system's red for the worst, then orange, yellow and green, blue for from lossless, grey when unknown, as macOS draws
// them in the palette's appearance. Words always say the tier too (PRD UI-4). Main thread only.
QColor qualityTierColor(int tier, const QPalette &palette);

#ifdef Q_OS_MACOS
// The system accent for Aqua, or lightened for Dark Aqua, in sRGB; invalid if AppKit can't give it. AppearanceMac.mm.
QColor macSystemAccentColor(bool dark);
#endif

} // namespace selecta::app
