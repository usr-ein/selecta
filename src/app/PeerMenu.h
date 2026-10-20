// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCoreApplication>
#include <QString>

#include <functional>

class QMenu;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// What every list of peers offers about one of them: browse their files (BRWS-1), write to them (NET-7) and the user
// lists (NET-8). Main thread only.
class PeerMenu
{
    Q_DECLARE_TR_FUNCTIONS(PeerMenu)

public:
    struct Handlers
    {
        std::function<void(const QString &)> browse;  // empty: no Browse action
        std::function<void(const QString &)> message; // empty: no Message action
    };

    // Adds the actions about `peer` to `menu`: Browse, Message, then a buddy, ignore and ban toggle each.
    static void addActions(QMenu &menu, const QString &peer, core::EngineHost &engine, const Handlers &handlers);
};

} // namespace selecta::app
