// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MatchCorrections.h"

#include "core/CollectionStore.h"
#include "match/Normalizer.h"
#include "storage/Database.h"

#include <algorithm>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

using storage::Statement;

QString kindName(CorrectionKind kind)
{
    switch (kind) {
    case CorrectionKind::SearchResult:
        return u"search"_s;
    case CorrectionKind::GetLine:
        return u"get"_s;
    case CorrectionKind::Supersede:
        return u"supersede"_s;
    }
    return u"search"_s;
}

CorrectionKind kindFrom(const QString &name)
{
    if (name == u"get"_s) {
        return CorrectionKind::GetLine;
    }
    return name == u"supersede"_s ? CorrectionKind::Supersede : CorrectionKind::SearchResult;
}

QString verdictName(Verdict verdict)
{
    return verdict == Verdict::Same ? u"same"_s : u"different"_s;
}

bool lengthsAgree(qint64 a, qint64 b)
{
    return a <= 0 || b <= 0 || std::abs(a - b) <= MatchCorrectionStore::kLengthToleranceMs;
}

bool agrees(const TrackIdentity &stored, const TrackIdentity &asked)
{
    return stored.key == asked.key && lengthsAgree(stored.durationMs, asked.durationMs);
}

// A stored pair is the asked one, either way round.
bool samePair(const TrackIdentity &a, const TrackIdentity &b, const TrackIdentity &first, const TrackIdentity &second)
{
    return (agrees(a, first) && agrees(b, second)) || (agrees(a, second) && agrees(b, first));
}

// The kinds whose verdicts hold in the context of `kind`: a wanted track against an owned copy, or supersede.
QString contextOf(CorrectionKind kind)
{
    return kind == CorrectionKind::Supersede ? u"('supersede')"_s : u"('search', 'get')"_s;
}

// The rows naming both keys, either way round, in the context of `kind`, newest first: id, a_key, a_ms, b_key, b_ms,
// verdict.
Statement pairRows(storage::Database &db, CorrectionKind kind, const TrackIdentity &first, const TrackIdentity &second)
{
    Statement query = db.prepare(u"SELECT id, a_key, a_ms, b_key, b_ms, verdict FROM match_corrections "
                                 "WHERE ((a_key = ?1 AND b_key = ?2) OR (a_key = ?2 AND b_key = ?1)) AND kind IN %1 "
                                 "ORDER BY created_at DESC, id DESC;"_s.arg(contextOf(kind)));
    if (query.isValid()) {
        query.bind(1, first.key).bind(2, second.key);
    }
    return query;
}

} // namespace

TrackIdentity trackIdentity(const QString &artist, const QString &title, qint64 durationMs)
{
    const match::TrackKeys keys = match::Normalizer::trackKeys(artist, title);
    if (keys.title.field.isEmpty()) {
        return {}; // nothing to tell this track from another by
    }
    return {keys.exactKey, std::max<qint64>(0, durationMs)};
}

TrackIdentity remoteIdentity(const match::ParsedRemote &parsed, int durationSec)
{
    if (parsed.unparseable || parsed.keys.title.field.isEmpty()) {
        return {};
    }
    return {parsed.keys.exactKey, qint64{std::max(0, durationSec)} * 1000};
}

TrackIdentity inboxIdentity(const library::InboxItem &item)
{
    const bool versioned = !item.version.isEmpty() && !item.title.contains(item.version, Qt::CaseInsensitive);
    return trackIdentity(item.artist, versioned ? u"%1 (%2)"_s.arg(item.title, item.version) : item.title,
                         item.durationMs);
}

TrackIdentity collectionIdentity(const collection::ScannedFile &file)
{
    return trackIdentity(file.artist, file.title, file.durationMs);
}

TrackIdentity libraryIdentity(storage::Database &db, quint64 id, match::Source source)
{
    if (source == match::Source::Inbox) {
        const std::optional<library::InboxItem> item = library::InboxStore(db).find(static_cast<qint64>(id));
        return item ? inboxIdentity(*item) : TrackIdentity{};
    }
    const std::optional<CollectionFile> file = CollectionStore(db).find(static_cast<qint64>(id));
    return file ? collectionIdentity(file->file) : TrackIdentity{};
}

bool MatchCorrectionStore::record(CorrectionKind kind, const TrackIdentity &a, const TrackIdentity &b, Verdict verdict,
                                  qint64 atMs)
{
    if (!a.isValid() || !b.isValid()) {
        return false;
    }
    storage::Transaction transaction(m_db);
    if (!transaction.isActive()) {
        return false;
    }
    // The user changed their mind, or said it again: one verdict per pair.
    QList<qint64> earlier;
    Statement rows = pairRows(m_db, kind, a, b);
    if (!rows.isValid()) {
        return false;
    }
    while (rows.step() == Statement::Step::Row) {
        if (samePair({rows.text(1), rows.int64(2)}, {rows.text(3), rows.int64(4)}, a, b)) {
            earlier.append(rows.int64(0));
        }
    }
    for (const qint64 id : std::as_const(earlier)) {
        Statement remove = m_db.prepare(u"DELETE FROM match_corrections WHERE id = ?1;");
        if (!remove.isValid() || !remove.bind(1, id).run()) {
            return false;
        }
    }
    Statement insert = m_db.prepare(u"INSERT INTO match_corrections (kind, a_key, a_ms, b_key, b_ms, verdict, "
                                    "created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    if (!insert.isValid()) {
        return false;
    }
    insert.bind(1, kindName(kind)).bind(2, a.key).bind(3, a.durationMs).bind(4, b.key).bind(5, b.durationMs);
    insert.bind(6, verdictName(verdict)).bind(7, atMs);
    return insert.run() && transaction.commit();
}

std::optional<Verdict> MatchCorrectionStore::verdict(CorrectionKind kind, const TrackIdentity &a,
                                                     const TrackIdentity &b)
{
    if (!a.isValid() || !b.isValid()) {
        return std::nullopt;
    }
    Statement rows = pairRows(m_db, kind, a, b);
    while (rows.isValid() && rows.step() == Statement::Step::Row) {
        if (samePair({rows.text(1), rows.int64(2)}, {rows.text(3), rows.int64(4)}, a, b)) {
            return rows.text(5) == u"same"_s ? Verdict::Same : Verdict::Different;
        }
    }
    return std::nullopt;
}

bool MatchCorrectionStore::isEmpty()
{
    Statement query = m_db.prepare(u"SELECT EXISTS (SELECT 1 FROM match_corrections);");
    return !query.isValid() || query.step() != Statement::Step::Row || query.int64(0) == 0;
}

QList<MatchCorrection> MatchCorrectionStore::all()
{
    QList<MatchCorrection> corrections;
    Statement query = m_db.prepare(u"SELECT id, kind, a_key, a_ms, b_key, b_ms, verdict, created_at "
                                   "FROM match_corrections ORDER BY created_at DESC, id DESC;");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        MatchCorrection correction;
        correction.id = query.int64(0);
        correction.kind = kindFrom(query.text(1));
        correction.a = {query.text(2), query.int64(3)};
        correction.b = {query.text(4), query.int64(5)};
        correction.verdict = query.text(6) == u"same"_s ? Verdict::Same : Verdict::Different;
        correction.createdAt = query.int64(7);
        corrections.append(correction);
    }
    return corrections;
}

} // namespace selecta::core
