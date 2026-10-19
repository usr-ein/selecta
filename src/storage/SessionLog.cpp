// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/SessionLog.h"

#include "storage/Database.h"

namespace selecta::storage {

int SessionLog::start(qint64 startedAtMs)
{
    Statement insert = m_db.prepare(u"INSERT INTO sessions (started_at) VALUES (?1);");
    if (!insert.isValid() || !insert.bind(1, startedAtMs).run())
        return -1;
    return static_cast<int>(m_db.lastInsertRowId());
}

bool SessionLog::end(int session, qint64 endedAtMs)
{
    Statement update = m_db.prepare(u"UPDATE sessions SET ended_at = ?1 WHERE id = ?2 AND ended_at IS NULL;");
    return update.isValid() && update.bind(1, endedAtMs).bind(2, session).run() && m_db.changes() == 1;
}

int SessionLog::latest()
{
    Statement query = m_db.prepare(u"SELECT COALESCE(MAX(id), 0) FROM sessions;");
    if (query.isValid() && query.step() == Statement::Step::Row)
        return static_cast<int>(query.int64(0));
    return 0;
}

QList<SessionRecord> SessionLog::recent(int limit)
{
    QList<SessionRecord> sessions;
    if (limit <= 0)
        return sessions;
    Statement query = m_db.prepare(u"SELECT id, started_at, ended_at FROM sessions ORDER BY id DESC LIMIT ?1;");
    if (!query.isValid())
        return sessions;
    query.bind(1, limit);
    while (query.step() == Statement::Step::Row) {
        SessionRecord record;
        record.number = static_cast<int>(query.int64(0));
        record.startedAtMs = query.int64(1);
        if (!query.isNull(2))
            record.endedAtMs = query.int64(2);
        sessions.append(record);
    }
    return sessions;
}

} // namespace selecta::storage
