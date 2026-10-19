// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QtTypes>

#include <optional>

namespace selecta::storage {

class Database;

struct SessionRecord
{
    int number = 0;
    qint64 startedAtMs = 0;
    std::optional<qint64> endedAtMs; // empty while running, or when Selecta didn't quit cleanly
};

// Sessions are numbered runs of Selecta, from launch to quit (PRD D8). History groups downloads by session.
// Uses the Database on its owning thread.
class SessionLog
{
public:
    explicit SessionLog(Database &db)
        : m_db(db)
    {}

    // Records a new session and returns its number, or -1 on failure.
    int start(qint64 startedAtMs);
    // Marks a running session as ended. False if it doesn't exist or has already ended.
    [[nodiscard]] bool end(int session, qint64 endedAtMs);

    // The number of the most recent session, or 0 if there is none.
    int latest();
    // Up to `limit` sessions, newest first.
    QList<SessionRecord> recent(int limit);

private:
    Database &m_db;
};

} // namespace selecta::storage
