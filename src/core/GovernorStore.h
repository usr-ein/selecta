// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDate>
#include <QObject>
#include <QString>

#include <functional>

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}

namespace selecta::core {

// Keeps the Governor's per-peer state across restarts, so quitting and relaunching never resets a limit:
// - each peer's daily totals (FAIR-2: the daily and weekly caps), in peer_days;
// - the back-offs peers' denials started (etiquette note §9.1), in peer_backoffs, as wall-clock deadlines.
//
// Threading: lives on the Governor's thread and uses a Database owned by that thread.
// Failure: a failed write is logged and dropped; the Governor's in-memory state stays authoritative for the session.
class GovernorStore : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>; // Unix time, ms

    // Starts saving the Governor's changes at once.
    GovernorStore(storage::Database &db, governor::Governor &governor, WallClock now = {}, QObject *parent = nullptr);

    // Loads the saved state into the Governor and deletes what has expired. Call once at start-up, before the network
    // starts. Returns false if the saved state couldn't be read: the Governor then starts without earlier totals and
    // back-offs, and the caller should log it.
    bool restore();

private:
    void savePeerDay(const QString &peer, QDate day, int files, qint64 bytes);
    void saveBackoff(const QString &peer, qint64 remainingMs, const QString &reason);

    storage::Database &m_db;
    governor::Governor &m_governor;
    WallClock m_now;
};

} // namespace selecta::core
