// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fixtures/TestUtil.h"
#include "slsk/PermitSource.h"

#include <QList>
#include <QObject>
#include <QTimer>

#include <utility>

namespace selecta::slsk::testing {

// A PermitSource for tests: grants or refuses (on a later event-loop turn, like core/, or at once), or holds requests
// until release(). Records every request.
class TestPermits : public PermitSource
{
public:
    enum class Mode {
        Grant,
        Refuse,
        Defer,
    };

    Mode mode = Mode::Grant;
    bool synchronous = false; // deliver inside requestPeerLookup() instead of on a later turn
    QList<std::pair<QString, ConnectPurpose>> requests;

    // The interface takes the callback by value so an implementation can keep it.
    void requestPeerLookup(const QString &username, ConnectPurpose purpose,
                           Callback deliver) override // clazy:exclude=function-args-by-ref
    {
        requests.append({username, purpose});
        switch (mode) {
        case Mode::Grant:
            if (synchronous) {
                deliver(permit(governor::Operation::PeerLookup));
            } else {
                QTimer::singleShot(0, &m_context, [deliver] { deliver(permit(governor::Operation::PeerLookup)); });
            }
            return;
        case Mode::Refuse:
            if (synchronous) {
                deliver(std::nullopt);
            } else {
                QTimer::singleShot(0, &m_context, [deliver] { deliver(std::nullopt); });
            }
            return;
        case Mode::Defer:
            m_deferred.append(deliver);
            return;
        }
    }

    // Grants every held request.
    void release()
    {
        const QList<Callback> held = std::exchange(m_deferred, {});
        for (const Callback &deliver : held) {
            deliver(permit(governor::Operation::PeerLookup));
        }
    }

    int held() const { return static_cast<int>(m_deferred.size()); }

    int requestsFor(const QString &username) const
    {
        return static_cast<int>(std::ranges::count_if(requests, [&](const auto &r) { return r.first == username; }));
    }

private:
    QObject m_context;
    QList<Callback> m_deferred;
};

} // namespace selecta::slsk::testing
