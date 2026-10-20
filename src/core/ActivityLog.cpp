// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ActivityLog.h"

#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcActivity, "selecta.core.activity")

using storage::Statement;

constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

bool isMetadataOperation(governor::Operation operation)
{
    switch (operation) {
    case governor::Operation::SpotifyRequest:
    case governor::Operation::MusicBrainzRequest:
    case governor::Operation::CoverArtRequest:
    case governor::Operation::DeezerRequest:
    case governor::Operation::YtDlpRun:
        return true;
    default:
        return false;
    }
}

} // namespace

ActivityLog::ActivityLog(storage::Database &db, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{}

void ActivityLog::recordDecision(governor::Operation operation, const QString &subject, governor::Refusal refusal,
                                 const QString &explanation)
{
    // A metadata request that goes ahead is logged once it finishes (GovernorPacer::report), with its outcome; only
    // the refusals are worth a line of their own.
    if (refusal == governor::Refusal::None && isMetadataOperation(operation)) {
        return;
    }
    record(governor::operationName(operation), subject,
           refusal == governor::Refusal::None ? u"allowed"_s : governor::refusalName(refusal), explanation);
}

ActivityEntry ActivityLog::record(const QString &operation, const QString &subject, const QString &outcome,
                                  const QString &explanation)
{
    ActivityEntry entry{.id = 0,
                        .atMs = m_now(),
                        .operation = operation,
                        .subject = subject,
                        .outcome = outcome,
                        .explanation = explanation};
    Statement insert = m_db.prepare(
        u"INSERT INTO activity (at, operation, subject, outcome, explanation) VALUES (?1, ?2, ?3, ?4, ?5);");
    if (!insert.isValid() || !insert.bind(1, entry.atMs)
                                  .bind(2, entry.operation)
                                  .bind(3, entry.subject)
                                  .bind(4, entry.outcome)
                                  .bind(5, entry.explanation)
                                  .run()) {
        qCWarning(lcActivity) << "couldn't record activity:" << m_db.lastError();
        return entry;
    }
    entry.id = m_db.lastInsertRowId();
    Q_EMIT recorded(entry);
    return entry;
}

int ActivityLog::prune()
{
    storage::Transaction transaction(m_db);
    if (!transaction.isActive()) {
        return -1;
    }
    Statement byAge = m_db.prepare(u"DELETE FROM activity WHERE at < ?1;");
    if (!byAge.isValid() || !byAge.bind(1, m_now() - (kKeepDays * kDayMs)).run()) {
        return -1;
    }
    int removed = m_db.changes();
    // Keep the newest kMaxRows by id (ids only grow).
    Statement byCount =
        m_db.prepare(u"DELETE FROM activity WHERE id <= (SELECT id FROM activity ORDER BY id DESC LIMIT 1 OFFSET ?1);");
    if (!byCount.isValid() || !byCount.bind(1, kMaxRows).run()) {
        return -1;
    }
    removed += m_db.changes();
    if (!transaction.commit()) {
        return -1;
    }
    return removed;
}

QList<ActivityEntry> ActivityLog::recent(storage::Database &db, int limit)
{
    QList<ActivityEntry> entries;
    Statement query =
        db.prepare(u"SELECT id, at, operation, subject, outcome, explanation FROM activity ORDER BY id DESC LIMIT ?1;");
    if (!query.isValid()) {
        return entries;
    }
    query.bind(1, std::max(0, limit));
    while (query.step() == Statement::Step::Row) {
        entries.append(ActivityEntry{.id = query.int64(0),
                                     .atMs = query.int64(1),
                                     .operation = query.text(2),
                                     .subject = query.text(3),
                                     .outcome = query.text(4),
                                     .explanation = query.text(5)});
    }
    return entries;
}

} // namespace selecta::core
