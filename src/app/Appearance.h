// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringView>

namespace selecta::app {

// Light / dark appearance (PRD UI-2). System follows macOS.
enum class Appearance { System, Light, Dark };

QString appearanceKey(Appearance appearance); // "system", "light", "dark"
Appearance appearanceFromKey(QStringView key, Appearance fallback = Appearance::System);

// Applies the appearance to the whole application. Main thread only.
void applyAppearance(Appearance appearance);

} // namespace selecta::app
