// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GovernorStore.h"

#include "governor/Governor.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcGovernorStore, "selecta.core.governorstore")

using storage::Statement;

// A peer writes the denial text, so keep what is stored short.
constexpr qsizetype kMaxReasonLength = 200;

QString dayKey(QDate day)
{
    return day.toString(Qt::ISODate);
}

} // namespace

GovernorStore::GovernorStore(storage::Database &db, governor::Governor &governor, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
    connect(&governor, &governor::Governor::peerDayChanged, this, &GovernorStore::savePeerDay);
    connect(&governor, &governor::Governor::peerBackoffStarted, this, &GovernorStore::saveBackoff);
}

bool GovernorStore::restore()
{
    const qint64 now = m_now();
    const QDate today = QDateTime::fromMSecsSinceEpoch(now).date();

    // What can no longer affect a limit goes first; failing to delete it is harmless, so it's only logged.
    Statement forgetDays = m_db.prepare(u"DELETE FROM peer_days WHERE day < ?1;");
    Statement forgetBackoffs = m_db.prepare(u"DELETE FROM peer_backoffs WHERE until_at <= ?1;");
    if (!forgetDays.isValid() || !forgetDays.bind(1, dayKey(today.addDays(-governor::PeerLedger::kDaysKept))).run() ||
        !forgetBackoffs.isValid() || !forgetBackoffs.bind(1, now).run()) {
        qCWarning(lcGovernorStore) << "couldn't delete expired peer state:" << m_db.lastError();
    }

    bool complete = true;
    Statement days = m_db.prepare(u"SELECT peer, day, files, bytes FROM peer_days;");
    if (!days.isValid()) {
        return false;
    }
    auto step = days.step();
    for (; step == Statement::Step::Row; step = days.step()) {
        const QDate day = QDate::fromString(days.text(1), Qt::ISODate);
        if (day.isValid()) {
            m_governor.restorePeerDay(
                days.text(0), day, governor::PeerDay{.files = static_cast<int>(days.int64(2)), .bytes = days.int64(3)});
        }
    }
    complete = complete && step == Statement::Step::Done;

    Statement backoffs = m_db.prepare(u"SELECT peer, until_at FROM peer_backoffs;");
    if (!backoffs.isValid()) {
        return false;
    }
    step = backoffs.step();
    for (; step == Statement::Step::Row; step = backoffs.step()) {
        m_governor.restorePeerBackoff(backoffs.text(0), base::Clock::Duration(backoffs.int64(1) - now));
    }
    complete = complete && step == Statement::Step::Done;
    return complete;
}

void GovernorStore::savePeerDay(const QString &peer, QDate day, int files, qint64 bytes)
{
    Statement upsert =
        m_db.prepare(u"INSERT INTO peer_days (peer, day, files, bytes) VALUES (?1, ?2, ?3, ?4) "
                     "ON CONFLICT (peer, day) DO UPDATE SET files = excluded.files, bytes = excluded.bytes;");
    if (!upsert.isValid() || !upsert.bind(1, peer)
                                  .bind(2, dayKey(day))
                                  .bind(3, std::max(0, files))
                                  .bind(4, std::max<qint64>(0, bytes))
                                  .run()) {
        qCWarning(lcGovernorStore) << "couldn't save peer totals:" << m_db.lastError();
    }
}

void GovernorStore::saveBackoff(const QString &peer, qint64 remainingMs, const QString &reason)
{
    if (remainingMs <= 0) {
        return;
    }
    // A longer back-off already saved wins, as it does in the ledger.
    Statement upsert =
        m_db.prepare(u"INSERT INTO peer_backoffs (peer, until_at, reason) VALUES (?1, ?2, ?3) "
                     "ON CONFLICT (peer) DO UPDATE SET until_at = excluded.until_at, reason = excluded.reason "
                     "WHERE excluded.until_at > peer_backoffs.until_at;");
    if (!upsert.isValid() ||
        !upsert.bind(1, peer).bind(2, m_now() + remainingMs).bind(3, reason.trimmed().left(kMaxReasonLength)).run()) {
        qCWarning(lcGovernorStore) << "couldn't save a peer back-off:" << m_db.lastError();
    }
}

} // namespace selecta::core
