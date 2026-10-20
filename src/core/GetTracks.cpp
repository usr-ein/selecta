// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GetTracks.h"

#include "governor/Limits.h"

#include "core/ReleaseJson.h"
#include "storage/Database.h"

#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

using storage::Statement;
using ReleaseState = GetLine::ReleaseState;
using FindState = GetLine::FindState;
using DownloadState = GetLine::DownloadState;

constexpr auto kColumns =
    u"id, position, raw, dedupe_key, artist, title, duration_hint_ms, isrc, label_hint, release_state, release_json, "
    "release_confidence, find_state, best_json, ticked, download_state, message, added_at";

QString releaseStateName(ReleaseState state)
{
    switch (state) {
    case ReleaseState::Pending:
        return u"pending"_s;
    case ReleaseState::Resolved:
        return u"resolved"_s;
    case ReleaseState::Ambiguous:
        return u"ambiguous"_s;
    case ReleaseState::None:
        return u"none"_s;
    case ReleaseState::Failed:
        return u"failed"_s;
    }
    return u"pending"_s;
}

ReleaseState releaseStateFrom(const QString &name)
{
    for (const ReleaseState state : {ReleaseState::Pending, ReleaseState::Resolved, ReleaseState::Ambiguous,
                                     ReleaseState::None, ReleaseState::Failed}) {
        if (releaseStateName(state) == name) {
            return state;
        }
    }
    return ReleaseState::Pending;
}

QString findStateName(FindState state)
{
    switch (state) {
    case FindState::Idle:
        return u"idle"_s;
    case FindState::Queued:
        return u"queued"_s;
    case FindState::Searching:
        return u"searching"_s;
    case FindState::Ready:
        return u"ready"_s;
    case FindState::Check:
        return u"check"_s;
    case FindState::NotFound:
        return u"notfound"_s;
    case FindState::Owned:
        return u"owned"_s;
    case FindState::InInbox:
        return u"inbox"_s;
    }
    return u"idle"_s;
}

FindState findStateFrom(const QString &name)
{
    for (const FindState state : {FindState::Idle, FindState::Queued, FindState::Searching, FindState::Ready,
                                  FindState::Check, FindState::NotFound, FindState::Owned, FindState::InInbox}) {
        if (findStateName(state) == name) {
            return state;
        }
    }
    return FindState::Idle;
}

QString downloadStateName(DownloadState state)
{
    switch (state) {
    case DownloadState::None:
        return u"none"_s;
    case DownloadState::Queued:
        return u"queued"_s;
    case DownloadState::Transferring:
        return u"transferring"_s;
    case DownloadState::Converting:
        return u"converting"_s;
    case DownloadState::Arrived:
        return u"arrived"_s;
    case DownloadState::Failed:
        return u"failed"_s;
    }
    return u"none"_s;
}

DownloadState downloadStateFrom(const QString &name)
{
    for (const DownloadState state : {DownloadState::None, DownloadState::Queued, DownloadState::Transferring,
                                      DownloadState::Converting, DownloadState::Arrived, DownloadState::Failed}) {
        if (downloadStateName(state) == name) {
            return state;
        }
    }
    return DownloadState::None;
}

void bindOptionalText(Statement &statement, int index, const QByteArray &text)
{
    if (text.isEmpty()) {
        statement.bindNull(index);
    } else {
        statement.bind(index, QString::fromUtf8(text));
    }
}

// Binds every column but id and position, from parameter 1 (raw) to 16 (added_at).
void bindLine(Statement &statement, const GetLine &line)
{
    statement.bind(1, line.raw).bind(2, line.dedupeKey).bind(3, line.artist).bind(4, line.title);
    if (line.durationHintMs) {
        statement.bind(5, *line.durationHintMs);
    } else {
        statement.bindNull(5);
    }
    statement.bind(6, line.isrc).bind(7, line.labelHint).bind(8, releaseStateName(line.releaseState));
    bindOptionalText(statement, 9, line.release ? releaseToText(*line.release) : QByteArray());
    statement.bind(10, line.releaseConfidence).bind(11, findStateName(line.findState));
    bindOptionalText(statement, 12, line.best ? fileChoiceToText(*line.best) : QByteArray());
    statement.bind(13, line.ticked ? 1 : 0)
        .bind(14, downloadStateName(line.downloadState))
        .bind(15, line.message)
        .bind(16, line.addedAt);
}

GetLine readLine(const Statement &row)
{
    GetLine line;
    line.id = row.int64(0);
    line.position = static_cast<int>(row.int64(1));
    line.raw = row.text(2);
    line.dedupeKey = row.text(3);
    line.artist = row.text(4);
    line.title = row.text(5);
    line.durationHintMs = row.isNull(6) ? std::nullopt : std::optional<qint64>(row.int64(6));
    line.isrc = row.text(7);
    line.labelHint = row.text(8);
    line.releaseState = releaseStateFrom(row.text(9));
    line.release = releaseFromText(row.text(10).toUtf8());
    line.releaseConfidence = row.text(11);
    line.findState = findStateFrom(row.text(12));
    line.best = fileChoiceFromText(row.text(13).toUtf8());
    line.ticked = row.int64(14) != 0;
    line.downloadState = downloadStateFrom(row.text(15));
    line.message = row.text(16);
    line.addedAt = row.int64(17);
    return line;
}

} // namespace

QList<GetLine> GetTracksStore::append(const QList<GetLine> &lines)
{
    QList<GetLine> added;
    storage::Transaction transaction(m_db);
    if (!transaction.isActive()) {
        return added;
    }
    int position = 0;
    {
        Statement last = m_db.prepare(u"SELECT COALESCE(MAX(position), -1) FROM get_lines;");
        if (!last.isValid() || last.step() != Statement::Step::Row) {
            return added;
        }
        position = static_cast<int>(last.int64(0)) + 1;
    }
    Statement insert = m_db.prepare(
        u"INSERT INTO get_lines (raw, dedupe_key, artist, title, duration_hint_ms, isrc, label_hint, release_state, "
        "release_json, release_confidence, find_state, best_json, ticked, download_state, message, added_at, "
        "position) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17) "
        "ON CONFLICT (dedupe_key) DO NOTHING;");
    if (!insert.isValid()) {
        return added;
    }
    for (const GetLine &line : lines) {
        insert.reset();
        bindLine(insert, line);
        insert.bind(17, position);
        if (!insert.run()) {
            return {};
        }
        if (m_db.changes() == 1) {
            GetLine stored = line;
            stored.id = m_db.lastInsertRowId();
            stored.position = position;
            added.append(stored);
            ++position;
        }
    }
    if (!transaction.commit()) {
        return {};
    }
    return added;
}

bool GetTracksStore::update(const GetLine &line)
{
    Statement update = m_db.prepare(
        u"UPDATE get_lines SET raw = ?1, dedupe_key = ?2, artist = ?3, title = ?4, duration_hint_ms = ?5, isrc = ?6, "
        "label_hint = ?7, release_state = ?8, release_json = ?9, release_confidence = ?10, find_state = ?11, "
        "best_json = ?12, ticked = ?13, download_state = ?14, message = ?15, added_at = ?16 WHERE id = ?17;");
    if (!update.isValid()) {
        return false;
    }
    bindLine(update, line);
    update.bind(17, line.id);
    return update.run() && m_db.changes() == 1;
}

bool GetTracksStore::remove(const QList<qint64> &ids)
{
    storage::Transaction transaction(m_db);
    Statement remove = m_db.prepare(u"DELETE FROM get_lines WHERE id = ?1;");
    if (!transaction.isActive() || !remove.isValid()) {
        return false;
    }
    for (const qint64 id : ids) {
        remove.reset();
        if (!remove.bind(1, id).run()) {
            return false;
        }
    }
    return transaction.commit();
}

bool GetTracksStore::clear()
{
    return m_db.exec(u"DELETE FROM get_lines;");
}

std::optional<GetLine> GetTracksStore::find(qint64 id)
{
    Statement query = m_db.prepare(u"SELECT %1 FROM get_lines WHERE id = ?1;"_s.arg(kColumns));
    if (query.isValid() && query.bind(1, id).step() == Statement::Step::Row) {
        return readLine(query);
    }
    return std::nullopt;
}

QList<GetLine> GetTracksStore::all()
{
    QList<GetLine> lines;
    Statement query = m_db.prepare(u"SELECT %1 FROM get_lines ORDER BY position, id;"_s.arg(kColumns));
    if (!query.isValid()) {
        return lines;
    }
    while (query.step() == Statement::Step::Row) {
        lines.append(readLine(query));
    }
    return lines;
}

int GetTracksStore::count()
{
    Statement query = m_db.prepare(u"SELECT COUNT(*) FROM get_lines;");
    return query.isValid() && query.step() == Statement::Step::Row ? static_cast<int>(query.int64(0)) : 0;
}

QByteArray fileChoiceToText(const FileChoice &choice)
{
    QJsonObject object{
        {u"peer"_s, choice.peer},         {u"remotePath"_s, QString::fromLatin1(choice.remotePath.toBase64())},
        {u"size"_s, choice.size},         {u"extension"_s, choice.extension},
        {u"freeSlot"_s, choice.freeSlot}, {u"queueLength"_s, choice.queueLength},
        {u"fit"_s, choice.fit},           {u"fitsRelease"_s, choice.fitsRelease}};
    if (choice.bitrate) {
        object.insert(u"bitrate"_s, *choice.bitrate);
    }
    if (choice.durationSec) {
        object.insert(u"durationSec"_s, *choice.durationSec);
    }
    if (choice.vbr) {
        object.insert(u"vbr"_s, *choice.vbr);
    }
    if (choice.sampleRate) {
        object.insert(u"sampleRate"_s, *choice.sampleRate);
    }
    if (choice.bitDepth) {
        object.insert(u"bitDepth"_s, *choice.bitDepth);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<FileChoice> fileChoiceFromText(const QByteArray &text)
{
    if (text.isEmpty()) {
        return std::nullopt;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(text, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const auto optionalInt = [&object](const QString &key) {
        const QJsonValue value = object.value(key);
        return value.isDouble() ? std::optional<int>(value.toInt()) : std::nullopt;
    };
    FileChoice choice;
    choice.peer = object.value(u"peer"_s).toString();
    choice.remotePath = QByteArray::fromBase64(object.value(u"remotePath"_s).toString().toLatin1());
    choice.size = object.value(u"size"_s).toInteger();
    choice.extension = object.value(u"extension"_s).toString();
    choice.bitrate = optionalInt(u"bitrate"_s);
    choice.durationSec = optionalInt(u"durationSec"_s);
    if (object.value(u"vbr"_s).isBool()) {
        choice.vbr = object.value(u"vbr"_s).toBool();
    }
    choice.sampleRate = optionalInt(u"sampleRate"_s);
    choice.bitDepth = optionalInt(u"bitDepth"_s);
    choice.freeSlot = object.value(u"freeSlot"_s).toBool();
    choice.queueLength = object.value(u"queueLength"_s).toInt();
    choice.fit = object.value(u"fit"_s).toDouble();
    choice.fitsRelease = object.value(u"fitsRelease"_s).toBool();
    return choice;
}

std::chrono::milliseconds findingTimeLeft(int searchesToStart, int searchesCollecting, int searchesAvailable,
                                          std::chrono::milliseconds nextIn)
{
    using namespace std::chrono;
    if (searchesToStart <= 0) {
        return searchesCollecting > 0 ? duration_cast<milliseconds>(governor::limits::kResultsIdle) : 0ms;
    }
    const int startNow = std::clamp(searchesAvailable - governor::limits::kGetTracksReservedSearches, 0, searchesToStart);
    const int later = searchesToStart - startNow;
    milliseconds untilLastStarts = 0ms;
    if (later > 0) {
        const milliseconds refill = duration_cast<milliseconds>(governor::limits::kSearchRefill);
        const milliseconds first = nextIn > 0ms ? std::min(nextIn, refill) : refill;
        untilLastStarts = first + (refill * (later - 1));
    }
    return untilLastStarts + duration_cast<milliseconds>(governor::limits::kResultsIdle);
}

} // namespace selecta::core
