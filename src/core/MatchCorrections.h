// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"
#include "library/InboxStore.h"
#include "match/RemotePathParser.h"

#include <QList>
#include <QString>

#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// A track as the user's corrections know it (PRD LIB-6): the matcher's identity of its artist, title and version
// (match::TrackKeys::exactKey) and its length. No file, path or id: a verdict on two tracks holds wherever their files
// are, so it survives rescans, filing and the same file arriving again.
struct TrackIdentity
{
    QString key;
    qint64 durationMs = 0; // 0: unknown

    bool isValid() const { return !key.isEmpty(); }
    friend bool operator==(const TrackIdentity &, const TrackIdentity &) = default;
};

// The identity of a track from its tags: the artist and the title as written (version text included). Pure.
TrackIdentity trackIdentity(const QString &artist, const QString &title, qint64 durationMs);
// The identity of a remote file from its parse (ParsedRemote::keys) and its announced length in seconds. Pure.
TrackIdentity remoteIdentity(const match::ParsedRemote &parsed, int durationSec);
// The identity of an Inbox item (its title with its version, as the match index has it) or a collection file. Pure.
TrackIdentity inboxIdentity(const library::InboxItem &item);
TrackIdentity collectionIdentity(const collection::ScannedFile &file);
// The identity of a library record (a collection file or an Inbox item, by its match::LibraryTrack id) from the
// database; invalid when it's gone. The Database's thread.
TrackIdentity libraryIdentity(storage::Database &db, quint64 id, match::Source source);

enum class CorrectionKind : quint8 {
    SearchResult, // a search or browse result against an owned track ("Not What I Have", "Same as What I Have")
    GetLine,      // a Get tracks line against an owned track ("Not What I Have, Find It")
    Supersede,    // a single against an album's track ("Not the Same Track")
};

enum class Verdict : quint8 {
    Same,      // the same track: a likely match counts as a sure one
    Different, // not the same track: the pair is no match at all
};

struct MatchCorrection
{
    qint64 id = 0;
    CorrectionKind kind = CorrectionKind::SearchResult;
    TrackIdentity a; // the search result, the line or the single
    TrackIdentity b; // the owned track or the album's track
    Verdict verdict = Verdict::Different;
    qint64 createdAt = 0; // Unix time, ms
};

// The user's corrections of matches (PRD LIB-6, table match_corrections). A verdict is about two tracks, whichever way
// round, within its context: one on a wanted track against an owned copy (a search result, a Get tracks line) holds
// for both; one on a single against an album's track holds for supersede only, since two different tracks can share a
// name and a length and a single's verdict mustn't hide that copy from searches. Lengths must agree within
// kLengthToleranceMs, or one be unknown, for a verdict to apply (another edit of the same name is another track); the
// newest verdict on a pair wins.
//
// Threading: the Database's thread (the engine thread). Failures are reported through return values and the
// database's lastError().
class MatchCorrectionStore
{
public:
    static constexpr qint64 kLengthToleranceMs = 3000; // the matcher's exact Δ

    explicit MatchCorrectionStore(storage::Database &db)
        : m_db(db)
    {}

    // Records a verdict on a pair, replacing earlier verdicts on the same pair in the same context. Fails (false) for
    // an invalid identity.
    [[nodiscard]] bool record(CorrectionKind kind, const TrackIdentity &a, const TrackIdentity &b, Verdict verdict,
                              qint64 atMs);
    // The user's verdict on two tracks, either way round, in the context of `kind`; nullopt when there is none.
    std::optional<Verdict> verdict(CorrectionKind kind, const TrackIdentity &a, const TrackIdentity &b);
    // Whether the user ever corrected a match: when not, nothing needs looking up.
    bool isEmpty();
    // Every correction, newest first.
    QList<MatchCorrection> all();

private:
    storage::Database &m_db;
};

} // namespace selecta::core
