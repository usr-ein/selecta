// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/InboxStore.h"

#include "storage/Database.h"

using namespace Qt::StringLiterals;

namespace selecta::library {
namespace {

using storage::Statement;

constexpr auto kColumns =
    u"id, kind, relative_path, state, error, artist, title, version, album, year, track_count, "
    "duration_ms, format, bitrate, sample_rate, bit_depth, size_bytes, source_format, tag_source, "
    "release_json, peer, remote_path, session, created_at, warning";

QString kindName(InboxItem::Kind kind)
{
    return kind == InboxItem::Kind::Album ? u"album"_s : u"single"_s;
}

QString stateName(InboxItem::State state)
{
    switch (state) {
    case InboxItem::State::Processing:
        return u"processing"_s;
    case InboxItem::State::Ready:
        return u"ready"_s;
    case InboxItem::State::Failed:
        return u"failed"_s;
    }
    return u"failed"_s;
}

InboxItem::State stateFrom(const QString &name)
{
    if (name == u"processing") {
        return InboxItem::State::Processing;
    }
    if (name == u"ready") {
        return InboxItem::State::Ready;
    }
    return InboxItem::State::Failed;
}

std::optional<int> optionalInt(const Statement &row, int column)
{
    return row.isNull(column) ? std::nullopt : std::optional<int>(static_cast<int>(row.int64(column)));
}

void bindOptional(Statement &statement, int index, std::optional<int> value)
{
    if (value) {
        statement.bind(index, *value);
    } else {
        statement.bindNull(index);
    }
}

void bindText(Statement &statement, int index, const QString &value, bool nullWhenEmpty)
{
    if (nullWhenEmpty && value.isEmpty()) {
        statement.bindNull(index);
    } else {
        statement.bind(index, value);
    }
}

// Binds every column except id, in kColumns order starting at parameter 1.
void bindItem(Statement &statement, const InboxItem &item)
{
    statement.bind(1, kindName(item.kind)).bind(2, item.relativePath).bind(3, stateName(item.state));
    bindText(statement, 4, item.error, true);
    statement.bind(5, item.artist).bind(6, item.title).bind(7, item.version).bind(8, item.album);
    bindOptional(statement, 9, item.year);
    bindOptional(statement, 10, item.trackCount);
    statement.bind(11, item.durationMs).bind(12, item.format);
    bindOptional(statement, 13, item.bitrate);
    bindOptional(statement, 14, item.sampleRate);
    bindOptional(statement, 15, item.bitDepth);
    statement.bind(16, std::max<qint64>(0, item.sizeBytes)).bind(17, item.sourceFormat).bind(18, item.tagSource);
    bindText(statement, 19, item.releaseJson, true);
    statement.bind(20, item.peer);
    if (item.remotePath.isNull()) {
        statement.bindNull(21);
    } else {
        statement.bind(21, item.remotePath);
    }
    bindOptional(statement, 22, item.session);
    statement.bind(23, item.createdAt).bind(24, item.warning);
}

InboxItem readItem(const Statement &row)
{
    InboxItem item;
    item.id = row.int64(0);
    item.kind = row.text(1) == u"album" ? InboxItem::Kind::Album : InboxItem::Kind::Single;
    item.relativePath = row.text(2);
    item.state = stateFrom(row.text(3));
    item.error = row.text(4);
    item.artist = row.text(5);
    item.title = row.text(6);
    item.version = row.text(7);
    item.album = row.text(8);
    item.year = optionalInt(row, 9);
    item.trackCount = optionalInt(row, 10);
    item.durationMs = row.int64(11);
    item.format = row.text(12);
    item.bitrate = optionalInt(row, 13);
    item.sampleRate = optionalInt(row, 14);
    item.bitDepth = optionalInt(row, 15);
    item.sizeBytes = row.int64(16);
    item.sourceFormat = row.text(17);
    item.tagSource = row.text(18);
    item.releaseJson = row.text(19);
    item.peer = row.text(20);
    item.remotePath = row.isNull(21) ? QByteArray() : row.blob(21);
    item.session = optionalInt(row, 22);
    item.createdAt = row.int64(23);
    item.warning = row.text(24);
    return item;
}

} // namespace

std::optional<qint64> InboxStore::add(const InboxItem &item)
{
    Statement insert = m_db.prepare(
        u"INSERT INTO inbox_items (kind, relative_path, state, error, artist, title, version, album, year, "
        "track_count, duration_ms, format, bitrate, sample_rate, bit_depth, size_bytes, source_format, tag_source, "
        "release_json, peer, remote_path, session, created_at, warning) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, "
        "?23, ?24);");
    if (!insert.isValid()) {
        return std::nullopt;
    }
    bindItem(insert, item);
    if (!insert.run()) {
        return std::nullopt;
    }
    return m_db.lastInsertRowId();
}

bool InboxStore::update(const InboxItem &item)
{
    Statement update = m_db.prepare(
        u"UPDATE inbox_items SET kind = ?1, relative_path = ?2, state = ?3, error = ?4, artist = ?5, title = ?6, "
        "version = ?7, album = ?8, year = ?9, track_count = ?10, duration_ms = ?11, format = ?12, bitrate = ?13, "
        "sample_rate = ?14, bit_depth = ?15, size_bytes = ?16, source_format = ?17, tag_source = ?18, "
        "release_json = ?19, peer = ?20, remote_path = ?21, session = ?22, created_at = ?23, warning = ?24 "
        "WHERE id = ?25;");
    if (!update.isValid()) {
        return false;
    }
    bindItem(update, item);
    update.bind(25, item.id);
    return update.run() && m_db.changes() == 1;
}

bool InboxStore::setState(qint64 id, InboxItem::State state, const QString &error)
{
    Statement update = m_db.prepare(u"UPDATE inbox_items SET state = ?1, error = ?2 WHERE id = ?3;");
    if (!update.isValid()) {
        return false;
    }
    update.bind(1, stateName(state));
    bindText(update, 2, error, true);
    update.bind(3, id);
    return update.run() && m_db.changes() == 1;
}

bool InboxStore::remove(qint64 id)
{
    Statement remove = m_db.prepare(u"DELETE FROM inbox_items WHERE id = ?1;");
    return remove.isValid() && remove.bind(1, id).run() && m_db.changes() == 1;
}

std::optional<InboxItem> InboxStore::find(qint64 id)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM inbox_items WHERE id = ?1;"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, id).step() == Statement::Step::Row) {
        return readItem(query);
    }
    return std::nullopt;
}

std::optional<InboxItem> InboxStore::findByPath(const QString &relativePath)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM inbox_items WHERE relative_path = ?1;"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, relativePath).step() == Statement::Step::Row) {
        return readItem(query);
    }
    return std::nullopt;
}

QList<InboxItem> InboxStore::all()
{
    QList<InboxItem> items;
    Statement query = m_db.prepare(u"SELECT %1 FROM inbox_items ORDER BY created_at DESC, id DESC;"_s.arg(kColumns));
    if (!query.isValid()) {
        return items;
    }
    while (query.step() == Statement::Step::Row) {
        items.append(readItem(query));
    }
    return items;
}

int InboxStore::count()
{
    Statement query = m_db.prepare(u"SELECT COUNT(*) FROM inbox_items;");
    if (query.isValid() && query.step() == Statement::Step::Row) {
        return static_cast<int>(query.int64(0));
    }
    return 0;
}

} // namespace selecta::library
