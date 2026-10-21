// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Decision.h"
#include "slsk/PermitSource.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <functional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// One line of the network activity log (PRD SAFE-4): what was asked or sent, about whom, and what happened.
struct ActivityEntry
{
    qint64 id = 0;
    qint64 atMs = 0;     // Unix time, ms
    QString operation;   // "search", "queue-download", "spotify", "server"…
    QString subject;     // the query, peer, room or request target; never a secret or message text
    QString outcome;     // "allowed", a refusal name ("rate-limited"), "ok", "HTTP 429"…
    QString explanation; // a sentence for people
};

// The network activity log (PRD SAFE-4): every governor decision, every metadata request and every server event,
// kept in the activity table for 30 days (at most kMaxRows rows) so the maintainer can see what Selecta did.
//
// Each line about a peer says who started it and why (describe()): "You looked up X's address to send them your
// search results…", "X is downloading … from you". Peer lookups carry their purpose (recordPeerLookup()); a lookup
// for one of our own requests is named after the request we made of that peer just before (browse, queue a
// download…), which the governor reported to recordDecision() moments earlier in the same call chain. A lookup to
// carry on a file transfer names its direction after the download or upload lines for that peer recorded within
// kTransferMemoryMs: your download from them, their download from you, or both; with none, it says either may be.
//
// Threading: lives on the engine thread with that thread's Database. recent() and describe() are static so the UI
// thread can read with its own connection; it then follows new lines through recorded(), keyed by id to drop a line
// seen twice.
// Failure: a line that can't be written is logged to the file log and dropped; logging never blocks the network.
class ActivityLog : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>; // Unix time, ms

    static constexpr int kKeepDays = 30;
    static constexpr int kMaxRows = 100'000;

    explicit ActivityLog(storage::Database &db, WallClock now = {}, QObject *parent = nullptr);

    // Records one line and returns it with its id, or an entry with id 0 if the write failed.
    ActivityEntry record(const QString &operation, const QString &subject, const QString &outcome,
                         const QString &explanation);
    // Records one governor decision; connect Governor::decided here. Peer lookups are skipped: they're recorded with
    // their purpose by recordPeerLookup() (core::GovernorPermits).
    void recordDecision(governor::Operation operation, const QString &subject, governor::Refusal refusal,
                        const QString &explanation);
    // Records the governor's decision on a peer lookup, with why Selecta wants to connect to `username`.
    void recordPeerLookup(const QString &username, slsk::ConnectPurpose purpose, governor::Refusal refusal,
                          const QString &explanation);

    // Drops lines older than kKeepDays and the oldest beyond kMaxRows. Returns how many went, or -1 on failure.
    int prune();

    // The newest `limit` lines, newest first.
    static QList<ActivityEntry> recent(storage::Database &db, int limit);

    // The line as a sentence for people: who started it and why for the lines about a peer, the stored explanation
    // for the others. Works on lines written by earlier versions too.
    static QString describe(const ActivityEntry &entry);

    // How long a request we made of a peer names the lookup that follows it (the same call chain: well under this).
    static constexpr qint64 kRequestMemoryMs = 5000;
    // At most this many peers are remembered. A new peer when full first drops the expired ones; if all are still
    // recent (a burst), the whole memory is cleared.
    static constexpr int kRequestMemoryPeers = 256;
    // How long a download or upload line names the direction of a transfer lookup for that peer. A transfer's
    // lookups follow its lines within seconds (an upload starts, then we open the file connection); a download
    // queued long ago at the peer's end is past knowing, and its lookup keeps the wording that names both.
    static constexpr qint64 kTransferMemoryMs = qint64{10} * 60 * 1000;
    // At most this many peers' transfers are remembered, dropped the same way as the requests.
    static constexpr int kTransferMemoryPeers = 256;

Q_SIGNALS:
    void recorded(const selecta::core::ActivityEntry &entry);

private:
    struct AskedOf
    {
        governor::Operation operation = governor::Operation::Browse;
        qint64 atMs = 0;
    };
    // The transfer lines seen for one peer, newest per direction (0: none).
    struct TransfersWith
    {
        qint64 downloadAtMs = 0;
        qint64 uploadAtMs = 0;
    };
    void remember(const QString &peer, governor::Operation operation);
    void rememberTransfer(const QString &peer, bool download);

    storage::Database &m_db;
    WallClock m_now;
    QHash<QString, AskedOf> m_lastAsked; // per peer: our latest allowed request, for the lookup that follows it
    QHash<QString, TransfersWith> m_lastTransfers; // per peer: the latest download and upload lines
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ActivityEntry)
