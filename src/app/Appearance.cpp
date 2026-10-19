// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Appearance.h"

#include <QGuiApplication>
#include <QStyleHints>

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

} // namespace selecta::app
