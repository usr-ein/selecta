// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/HistoryStore.h"

#include "storage/Database.h"

using namespace Qt::StringLiterals;

namespace selecta::library {
namespace {

using storage::Statement;
using Kind = HistoryEntry::Kind;
using Whereabouts = HistoryEntry::Whereabouts;

constexpr auto kColumns = u"id, session, completed_at, kind, peer, remote_path, size_bytes, quality, artist, title, "
                          "version, inbox_item, whereabouts, filed_path";

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::Single:
        return u"single"_s;
    case Kind::Album:
        return u"album"_s;
    case Kind::Preview:
        return u"preview"_s;
    }
    return u"single"_s;
}

Kind kindFrom(const QString &name)
{
    if (name == u"album") {
        return Kind::Album;
    }
    if (name == u"preview") {
        return Kind::Preview;
    }
    return Kind::Single;
}

QString whereaboutsName(Whereabouts whereabouts)
{
    switch (whereabouts) {
    case Whereabouts::Inbox:
        return u"inbox"_s;
    case Whereabouts::Filed:
        return u"filed"_s;
    case Whereabouts::Trashed:
        return u"trashed"_s;
    case Whereabouts::Preview:
        return u"preview"_s;
    }
    return u"inbox"_s;
}

Whereabouts whereaboutsFrom(const QString &name)
{
    if (name == u"filed") {
        return Whereabouts::Filed;
    }
    if (name == u"trashed") {
        return Whereabouts::Trashed;
    }
    if (name == u"preview") {
        return Whereabouts::Preview;
    }
    return Whereabouts::Inbox;
}

std::optional<qint64> optionalInt64(const Statement &row, int column)
{
    return row.isNull(column) ? std::nullopt : std::optional<qint64>(row.int64(column));
}

void bindOptional(Statement &statement, int index, std::optional<qint64> value)
{
    if (value) {
        statement.bind(index, *value);
    } else {
        statement.bindNull(index);
    }
}

void bindFiledPath(Statement &statement, int index, Whereabouts whereabouts, const QString &filedPath)
{
    if (whereabouts == Whereabouts::Filed && !filedPath.isEmpty()) {
        statement.bind(index, filedPath);
    } else {
        statement.bindNull(index);
    }
}

HistoryEntry readEntry(const Statement &row)
{
    HistoryEntry entry;
    entry.id = row.int64(0);
    entry.session = optionalInt64(row, 1);
    entry.completedAt = row.int64(2);
    entry.kind = kindFrom(row.text(3));
    entry.peer = row.text(4);
    entry.remotePath = row.blob(5);
    entry.sizeBytes = row.int64(6);
    entry.quality = row.text(7);
    entry.artist = row.text(8);
    entry.title = row.text(9);
    entry.version = row.text(10);
    entry.inboxItem = optionalInt64(row, 11);
    entry.whereabouts = whereaboutsFrom(row.text(12));
    entry.filedPath = row.text(13);
    return entry;
}

QList<HistoryEntry> readAll(Statement &query)
{
    QList<HistoryEntry> entries;
    while (query.step() == Statement::Step::Row) {
        entries.append(readEntry(query));
    }
    return entries;
}

} // namespace

std::optional<qint64> HistoryStore::add(const HistoryEntry &entry)
{
    Statement insert = m_db.prepare(
        u"INSERT INTO history (session, completed_at, kind, peer, remote_path, size_bytes, quality, artist, title, "
        "version, inbox_item, whereabouts, filed_path) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13);");
    if (!insert.isValid()) {
        return std::nullopt;
    }
    bindOptional(insert, 1, entry.session);
    insert.bind(2, entry.completedAt).bind(3, kindName(entry.kind)).bind(4, entry.peer);
    // remote_path is NOT NULL: a null QByteArray would bind as NULL.
    insert.bind(5, entry.remotePath.isNull() ? QByteArray("") : entry.remotePath);
    insert.bind(6, std::max<qint64>(0, entry.sizeBytes))
        .bind(7, entry.quality)
        .bind(8, entry.artist)
        .bind(9, entry.title)
        .bind(10, entry.version);
    bindOptional(insert, 11, entry.inboxItem);
    insert.bind(12, whereaboutsName(entry.whereabouts));
    bindFiledPath(insert, 13, entry.whereabouts, entry.filedPath);
    if (!insert.run()) {
        return std::nullopt;
    }
    return m_db.lastInsertRowId();
}

bool HistoryStore::setWhereabouts(qint64 id, Whereabouts whereabouts, const QString &filedPath)
{
    Statement update = m_db.prepare(u"UPDATE history SET whereabouts = ?1, filed_path = ?2 WHERE id = ?3;");
    if (!update.isValid()) {
        return false;
    }
    update.bind(1, whereaboutsName(whereabouts));
    bindFiledPath(update, 2, whereabouts, filedPath);
    return update.bind(3, id).run() && m_db.changes() == 1;
}

int HistoryStore::setWhereaboutsForInboxItem(qint64 inboxItem, Whereabouts whereabouts, const QString &filedPath)
{
    Statement update = m_db.prepare(u"UPDATE history SET whereabouts = ?1, filed_path = ?2 WHERE inbox_item = ?3;");
    if (!update.isValid()) {
        return -1;
    }
    update.bind(1, whereaboutsName(whereabouts));
    bindFiledPath(update, 2, whereabouts, filedPath);
    return update.bind(3, inboxItem).run() ? m_db.changes() : -1;
}

QList<qint64> HistoryStore::idsForInboxItem(qint64 inboxItem)
{
    QList<qint64> ids;
    Statement query = m_db.prepare(u"SELECT id FROM history WHERE inbox_item = ?1 ORDER BY id;"_s);
    if (query.isValid()) {
        query.bind(1, inboxItem);
        while (query.step() == Statement::Step::Row) {
            ids.append(query.int64(0));
        }
    }
    return ids;
}

int HistoryStore::returnToInbox(const QList<qint64> &ids, qint64 inboxItem)
{
    Statement update =
        m_db.prepare(u"UPDATE history SET whereabouts = ?1, filed_path = NULL, inbox_item = ?2 WHERE id = ?3;"_s);
    if (!update.isValid()) {
        return -1;
    }
    int changed = 0;
    for (const qint64 id : ids) {
        update.reset();
        update.bind(1, whereaboutsName(Whereabouts::Inbox)).bind(2, inboxItem).bind(3, id);
        if (!update.run()) {
            return -1;
        }
        changed += m_db.changes();
    }
    return changed;
}

std::optional<HistoryEntry> HistoryStore::find(qint64 id)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM history WHERE id = ?1;"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, id).step() == Statement::Step::Row) {
        return readEntry(query);
    }
    return std::nullopt;
}

QList<HistoryEntry> HistoryStore::forSession(qint64 session)
{
    Statement query =
        m_db.prepare(u"SELECT %1 FROM history WHERE session = ?1 ORDER BY completed_at, id;"_s.arg(kColumns));
    if (!query.isValid()) {
        return {};
    }
    query.bind(1, session);
    return readAll(query);
}

QList<HistoryEntry> HistoryStore::recent(int limit)
{
    Statement query =
        m_db.prepare(u"SELECT %1 FROM history ORDER BY completed_at DESC, id DESC LIMIT ?1;"_s.arg(kColumns));
    if (!query.isValid()) {
        return {};
    }
    query.bind(1, std::max(0, limit));
    return readAll(query);
}

QList<HistoryEntry> HistoryStore::recentlyFiled(int limit)
{
    Statement query = m_db.prepare(
        u"SELECT %1 FROM history WHERE whereabouts = 'filed' ORDER BY completed_at DESC, id DESC LIMIT ?1;"_s.arg(
            kColumns));
    if (!query.isValid()) {
        return {};
    }
    query.bind(1, std::max(0, limit));
    return readAll(query);
}

QList<HistoryEntry> HistoryStore::downloads()
{
    Statement query =
        m_db.prepare(u"SELECT %1 FROM history WHERE kind <> 'preview' ORDER BY completed_at, id;"_s.arg(kColumns));
    if (!query.isValid()) {
        return {};
    }
    return readAll(query);
}

QList<PeerTotals> HistoryStore::peerTotals(std::optional<qint64> session)
{
    QList<PeerTotals> totals;
    Statement query = m_db.prepare(
        u"SELECT peer, count(*), coalesce(sum(size_bytes), 0), coalesce(sum(session IS ?1), 0), max(completed_at) "
        u"FROM history WHERE kind <> 'preview' GROUP BY peer ORDER BY max(completed_at) DESC, peer;"_s);
    if (!query.isValid()) {
        return totals;
    }
    bindOptional(query, 1, session);
    while (query.step() == Statement::Step::Row) {
        totals.append(PeerTotals{.peer = query.text(0),
                                 .files = static_cast<int>(query.int64(1)),
                                 .bytes = query.int64(2),
                                 .sessionFiles = session ? static_cast<int>(query.int64(3)) : 0,
                                 .lastMs = query.int64(4)});
    }
    return totals;
}

bool HistoryStore::contains(const QString &peer, const QByteArray &remotePath)
{
    Statement query = m_db.prepare(u"SELECT 1 FROM history WHERE peer = ?1 AND remote_path = ?2 LIMIT 1;");
    return query.isValid() && query.bind(1, peer).bind(2, remotePath).step() == Statement::Step::Row;
}

} // namespace selecta::library
