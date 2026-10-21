// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/PeerMenu.h"

#include "app/Format.h"
#include "core/EngineHost.h"

#include <QMenu>

namespace selecta::app {

void PeerMenu::addActions(QMenu &menu, const QString &peer, core::EngineHost &engine, const Handlers &handlers)
{
    if (peer.isEmpty()) {
        return;
    }
    // A peer can be named anything: an "&" in the name stays an "&" in the menu, not a shortcut.
    const QString name = format::noMnemonic(peer);
    if (handlers.browse) {
        menu.addAction(tr("Browse %1's Files").arg(name), &menu, [browse = handlers.browse, peer] { browse(peer); });
    }
    if (handlers.message) {
        menu.addAction(tr("Message %1…").arg(name), &menu, [message = handlers.message, peer] { message(peer); });
    }
    menu.addSeparator();
    const std::optional<core::UserList> listed = engine.listOf(peer);
    const auto toggle = [&](core::UserList list, const QString &add, const QString &remove) {
        const bool on = listed == list;
        menu.addAction(on ? remove : add, &menu, [&engine, peer, list, on] {
            engine.setUserListed(peer, on ? std::nullopt : std::optional(list));
        });
    };
    toggle(core::UserList::Buddy, tr("Add %1 to Buddies").arg(name), tr("Remove %1 from Buddies").arg(name));
    toggle(core::UserList::Ignored, tr("Ignore %1's Messages").arg(name), tr("Stop Ignoring %1").arg(name));
    toggle(core::UserList::Banned, tr("Ban %1 from Your Shares").arg(name), tr("Unban %1").arg(name));
}

} // namespace selecta::app
