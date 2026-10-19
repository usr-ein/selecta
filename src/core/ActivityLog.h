// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Decision.h"

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
// Threading: lives on the engine thread with that thread's Database. recent() is static so the UI thread can read
// with its own connection; it then follows new lines through recorded(), keyed by id to drop a line seen twice.
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
    // Records one governor decision; connect Governor::decided here.
    void recordDecision(governor::Operation operation, const QString &subject, governor::Refusal refusal,
                        const QString &explanation);

    // Drops lines older than kKeepDays and the oldest beyond kMaxRows. Returns how many went, or -1 on failure.
    int prune();

    // The newest `limit` lines, newest first.
    static QList<ActivityEntry> recent(storage::Database &db, int limit);

Q_SIGNALS:
    void recorded(const selecta::core::ActivityEntry &entry);

private:
    storage::Database &m_db;
    WallClock m_now;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ActivityEntry)
