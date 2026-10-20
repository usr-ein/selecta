// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Downloads.h"

#include "core/ReleaseJson.h"
#include "storage/Database.h"

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

using storage::Statement;
using State = Download::State;

constexpr std::array kStates{State::Waiting, State::Queued, State::Transferring, State::Converting,
                             State::Arrived, State::Failed, State::Cancelled};

const QString kColumns = u"id, peer, remote_path, size, file_json, release_json, get_line, state, message, attempts, "
                         "retry_at, received, local_path, inbox_item, created_at, updated_at"_s;

State stateFrom(const QString &name)
{
    for (const State state : kStates) {
        if (downloadStateName(state) == name) {
            return state;
        }
    }
    return State::Failed; // a damaged row never looks like work to resume
}

void bindOptional(Statement &statement, int index, std::optional<qint64> value)
{
    if (value) {
        statement.bind(index, *value);
    } else {
        statement.bindNull(index);
    }
}

void bindOptionalText(Statement &statement, int index, const QByteArray &text)
{
    if (text.isEmpty()) {
        statement.bindNull(index);
    } else {
        statement.bind(index, QString::fromUtf8(text));
    }
}

// Binds every column but id, from parameter 1 (peer) to 15 (updated_at).
void bindDownload(Statement &statement, const Download &download)
{
    statement.bind(1, download.peer).bind(2, download.remotePath).bind(3, download.size);
    bindOptionalText(statement, 4, download.file ? fileChoiceToText(*download.file) : QByteArray());
    bindOptionalText(statement, 5, download.release ? releaseToText(*download.release) : QByteArray());
    bindOptional(statement, 6, download.getLine);
    statement.bind(7, downloadStateName(download.state)).bind(8, download.message).bind(9, download.attempts);
    bindOptional(statement, 10, download.retryAtMs);
    statement.bind(11, download.received).bind(12, download.localPath);
    bindOptional(statement, 13, download.inboxItem);
    statement.bind(14, download.createdAt).bind(15, download.updatedAt);
}

std::optional<qint64> optionalInt(const Statement &row, int column)
{
    return row.isNull(column) ? std::nullopt : std::optional<qint64>(row.int64(column));
}

Download readDownload(const Statement &row)
{
    Download download;
    download.id = row.int64(0);
    download.peer = row.text(1);
    download.remotePath = row.blob(2);
    download.size = row.int64(3);
    download.file = fileChoiceFromText(row.text(4).toUtf8());
    download.release = releaseFromText(row.text(5).toUtf8());
    download.getLine = optionalInt(row, 6);
    download.state = stateFrom(row.text(7));
    download.message = row.text(8);
    download.attempts = static_cast<int>(row.int64(9));
    download.retryAtMs = optionalInt(row, 10);
    download.received = row.int64(11);
    download.localPath = row.text(12);
    download.inboxItem = optionalInt(row, 13);
    download.createdAt = row.int64(14);
    download.updatedAt = row.int64(15);
    return download;
}

} // namespace

QString Download::remoteFileName() const
{
    const QString path = QString::fromUtf8(remotePath);
    const qsizetype slash = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return path.mid(slash + 1);
}

QString downloadStateName(Download::State state)
{
    switch (state) {
    case State::Waiting:
        return u"waiting"_s;
    case State::Queued:
        return u"queued"_s;
    case State::Transferring:
        return u"transferring"_s;
    case State::Converting:
        return u"converting"_s;
    case State::Arrived:
        return u"arrived"_s;
    case State::Failed:
        return u"failed"_s;
    case State::Cancelled:
        return u"cancelled"_s;
    }
    return u"failed"_s;
}

std::optional<qint64> DownloadStore::add(const Download &download)
{
    Statement insert = m_db.prepare(
        u"INSERT INTO downloads (peer, remote_path, size, file_json, release_json, get_line, state, message, attempts, "
        "retry_at, received, local_path, inbox_item, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15);");
    if (!insert.isValid()) {
        return std::nullopt;
    }
    bindDownload(insert, download);
    if (!insert.run()) {
        return std::nullopt; // the same file is already being downloaded (unique index), or a database error
    }
    return m_db.lastInsertRowId();
}

bool DownloadStore::update(const Download &download)
{
    Statement update = m_db.prepare(
        u"UPDATE downloads SET peer = ?1, remote_path = ?2, size = ?3, file_json = ?4, release_json = ?5, "
        "get_line = ?6, state = ?7, message = ?8, attempts = ?9, retry_at = ?10, received = ?11, local_path = ?12, "
        "inbox_item = ?13, created_at = ?14, updated_at = ?15 WHERE id = ?16;");
    if (!update.isValid()) {
        return false;
    }
    bindDownload(update, download);
    update.bind(16, download.id);
    return update.run() && m_db.changes() == 1;
}

std::optional<Download> DownloadStore::find(qint64 id)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM downloads WHERE id = ?1;"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, id).step() == Statement::Step::Row) {
        return readDownload(query);
    }
    return std::nullopt;
}

std::optional<Download> DownloadStore::findUnfinished(const QString &peer, const QByteArray &remotePath)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM downloads WHERE peer = ?1 AND remote_path = ?2 "
                                   "AND state NOT IN ('arrived', 'failed', 'cancelled');"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, peer).bind(2, remotePath).step() == Statement::Step::Row) {
        return readDownload(query);
    }
    return std::nullopt;
}

QList<Download> DownloadStore::unfinished()
{
    QList<Download> downloads;
    Statement query = m_db.prepare(u"SELECT %1 FROM downloads WHERE state NOT IN ('arrived', 'failed', 'cancelled') "
                                   "ORDER BY created_at, id;"_s.arg(kColumns));
    while (query.isValid() && query.step() == Statement::Step::Row) {
        downloads.append(readDownload(query));
    }
    return downloads;
}

QList<Download> DownloadStore::forLine(qint64 lineId)
{
    QList<Download> downloads;
    Statement query =
        m_db.prepare(u"SELECT %1 FROM downloads WHERE get_line = ?1 ORDER BY created_at, id;"_s.arg(kColumns));
    if (query.isValid()) {
        query.bind(1, lineId);
        while (query.step() == Statement::Step::Row) {
            downloads.append(readDownload(query));
        }
    }
    return downloads;
}

int DownloadStore::pruneFinished(qint64 beforeMs)
{
    Statement prune =
        m_db.prepare(u"DELETE FROM downloads WHERE state IN ('arrived', 'failed', 'cancelled') AND updated_at < ?1;");
    if (!prune.isValid() || !prune.bind(1, beforeMs).run()) {
        return -1;
    }
    return m_db.changes();
}

} // namespace selecta::core
