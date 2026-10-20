// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Albums.h"

#include "core/ReleaseJson.h"
#include "storage/Database.h"

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

using storage::Statement;
using State = AlbumDownload::State;

constexpr std::array kStates{State::Listing, State::Downloading, State::Converting, State::Arrived,
                             State::Partial, State::Failed,      State::Cancelled};

const QString kColumns = u"id, peer, folder, release_json, state, message, inbox_item, created_at, updated_at"_s;

State stateFrom(const QString &name)
{
    for (const State state : kStates) {
        if (albumStateName(state) == name) {
            return state;
        }
    }
    return State::Failed; // a damaged row never looks like work to resume
}

void bind(Statement &statement, const AlbumDownload &album)
{
    statement.bind(1, album.peer).bind(2, album.folder);
    if (album.release) {
        statement.bind(3, QString::fromUtf8(releaseToText(*album.release)));
    } else {
        statement.bindNull(3);
    }
    statement.bind(4, albumStateName(album.state)).bind(5, album.message);
    if (album.inboxItem) {
        statement.bind(6, *album.inboxItem);
    } else {
        statement.bindNull(6);
    }
    statement.bind(7, album.createdAt).bind(8, album.updatedAt);
}

AlbumDownload read(const Statement &row)
{
    AlbumDownload album;
    album.id = row.int64(0);
    album.peer = row.text(1);
    album.folder = row.blob(2);
    album.release = releaseFromText(row.text(3).toUtf8());
    album.state = stateFrom(row.text(4));
    album.message = row.text(5);
    if (!row.isNull(6)) {
        album.inboxItem = row.int64(6);
    }
    album.createdAt = row.int64(7);
    album.updatedAt = row.int64(8);
    return album;
}

} // namespace

QString AlbumDownload::folderName() const
{
    const QString path = QString::fromUtf8(folder);
    const qsizetype separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return path.mid(separator + 1);
}

QString albumStateName(AlbumDownload::State state)
{
    switch (state) {
    case State::Listing:
        return u"listing"_s;
    case State::Downloading:
        return u"downloading"_s;
    case State::Converting:
        return u"converting"_s;
    case State::Arrived:
        return u"arrived"_s;
    case State::Partial:
        return u"partial"_s;
    case State::Failed:
        return u"failed"_s;
    case State::Cancelled:
        return u"cancelled"_s;
    }
    return u"failed"_s;
}

std::optional<qint64> AlbumStore::add(const AlbumDownload &album)
{
    Statement insert = m_db.prepare(u"INSERT INTO album_downloads (peer, folder, release_json, state, message, "
                                    "inbox_item, created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");
    if (!insert.isValid()) {
        return std::nullopt;
    }
    bind(insert, album);
    if (!insert.run()) {
        return std::nullopt;
    }
    return m_db.lastInsertRowId();
}

bool AlbumStore::update(const AlbumDownload &album)
{
    Statement update = m_db.prepare(u"UPDATE album_downloads SET peer = ?1, folder = ?2, release_json = ?3, "
                                    "state = ?4, message = ?5, inbox_item = ?6, created_at = ?7, updated_at = ?8 "
                                    "WHERE id = ?9;");
    if (!update.isValid()) {
        return false;
    }
    bind(update, album);
    update.bind(9, album.id);
    return update.run() && m_db.changes() == 1;
}

std::optional<AlbumDownload> AlbumStore::find(qint64 id)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM album_downloads WHERE id = ?1;"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, id).step() == Statement::Step::Row) {
        return read(query);
    }
    return std::nullopt;
}

QList<AlbumDownload> AlbumStore::unfinished()
{
    QList<AlbumDownload> albums;
    Statement query = m_db.prepare(u"SELECT %1 FROM album_downloads WHERE state NOT IN ('arrived', 'partial', "
                                   "'failed', 'cancelled') ORDER BY created_at, id;"_s.arg(kColumns));
    while (query.isValid() && query.step() == Statement::Step::Row) {
        albums.append(read(query));
    }
    return albums;
}

} // namespace selecta::core
