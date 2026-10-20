// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/MatchIndex.h"
#include "match/RemotePathParser.h"
#include "match/Types.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace selecta::match {

// Scoring constants (note §9, "Proposed defaults"; initial values, to tune on the labelled corpus, ADR-008).
inline constexpr double kWeightTitle = 0.45;
inline constexpr double kWeightArtist = 0.30;
inline constexpr double kWeightDuration = 0.20;
inline constexpr double kWeightAlbum = 0.05;
inline constexpr double kExactMinScore = 0.97; // second exact route: score, title ≥ 0.97, artist ≥ 0.95, s_dur = 1
inline constexpr double kExactMinTitle = 0.97;
inline constexpr double kExactMinArtist = 0.95;
inline constexpr double kLikelyMinScore = 0.85;
inline constexpr double kLikelyMinTitle = 0.85;
inline constexpr double kLikelyMinArtist = 0.70;
inline constexpr double kContradictionMinTitle = 0.9; // same title, clearly different artist: × 0.5 (covers)
inline constexpr double kContradictionMaxArtist = 0.5;
inline constexpr double kContradictionFactor = 0.5;
inline constexpr double kFarLengthFactor = 0.4;  // Δ beyond t_far
inline constexpr double kWholeNameFactor = 0.9;  // the remote name had no segment split
inline constexpr double kMaxUnexplained = 0.3;   // u above this: another title that contains ours (§17.4)
inline constexpr qsizetype kMaxCandidates = 200; // retrieved records scored per lookup (note §10)

// A search result as far as matching is concerned.
struct RemoteFile
{
    QString peer;     // the user sharing it; empty when unknown
    QString path;     // the virtual path exactly as received
    quint64 size = 0; // bytes
    RemoteAttributes attributes;
};

// The evidence behind a decision (note §9). Missing evidence is absent, not zero: its weight leaves the denominator.
struct FieldScores
{
    double title = 0.0;
    std::optional<double> artist;
    std::optional<double> duration; // s_dur
    std::optional<double> album;
    double base = 0.0;       // weighted mean of the available fields
    double multiplier = 1.0; // product of the discriminators (version conflict, artist contradiction, length)
    double score = 0.0;      // base × multiplier
};

// One library record as a match for a remote file or a local track.
struct RecordMatch
{
    quint64 id = 0; // LibraryTrack::id
    Source source = Source::Collection;
    quint64 albumId = 0; // LibraryTrack::albumId: DL-11 "already on an album you own"
    Tier tier = Tier::None;
    Relation relation = Relation::None;
    VersionCompatibility version = VersionCompatibility::Same;
    FieldScores scores;
    QStringList explanation; // this record's scoring, when MatchOptions::explain

    bool isSet() const { return relation != Relation::None; }
};

struct MatchOptions
{
    bool explain = false; // fill the explanations (the inspector); decisions are identical either way
};

// Ownership of one remote file (PRD SRCH-11, LIB-3, LIB-4). Badge precedence (note §9): collection exact → upgrade →
// collection likely → Inbox → … → history → other version.
struct MatchResult
{
    RecordMatch collection;   // best collection record at tier ≥ likely (relation Same or Upgrade); unset if none
    RecordMatch inbox;        // the same for the Inbox
    RecordMatch otherVersion; // best record with relation OtherVersion (collection first); unset if none
    QList<quint64> history;   // history entries that are this very file: same (size, basename) or (peer, path)
    bool mix = false;         // longer than 20 minutes: only history was checked (note §7)
    quint64 generation = 0;   // MatchIndex::generation() of the snapshot used
    QStringList explanation;  // the whole decision, when MatchOptions::explain
};

// A local track looked up against the library (supersede candidates, the duplicate guard): tags and exact length.
struct LocalTrackQuery
{
    QString artist;
    QString title;
    QString album;
    qint64 durationMs = 0;
    AudioQuality quality;
    quint64 excludeAlbumId = 0; // records of this album are skipped (the album being checked); 0 = none
};

// Ownership matching (note §9, §10, §17.5).
//
// Per remote file: exact history lookups; for files of 20 minutes or less, candidate retrieval from the snapshot
// (squashed-key and token postings, typos through the fuzzy token index, at most kMaxCandidates records); then every
// candidate is scored twice, once on the parsed fields (§9) and once as a position-free token bag against the
// candidate's tags (§17.5 B), keeping the better title and artist evidence; then the tier rules and caps:
//
//   exact   no typo and no extra bracket text, compatible versions, and either equal keys with Δ ≤ 3 s (or no length
//           on one side with equal explicit versions), or score ≥ 0.97 with title ≥ 0.97, artist ≥ 0.95, s_dur = 1;
//   likely  score ≥ 0.85, title ≥ 0.85 and artist ≥ 0.70 (or no remote artist but the folder artist or album agrees),
//           no version conflict;
//   caps    an Unsure version, Unspecified vs Original beyond 3 s, an ambiguous parse, or two likely candidates of
//           different versions: at most likely.
//
// Threading: immutable; match() may be called concurrently from any number of threads. Ownership: keeps the snapshot
// alive. Failure modes: none; a null index behaves as an empty library (everything is None).
class Matcher final
{
public:
    explicit Matcher(std::shared_ptr<const MatchIndex> index);

    const std::shared_ptr<const MatchIndex> &index() const { return m_index; }

    // `parsed` is RemotePathParser's result for file.path (parsed with the folder's siblings and
    // ParseContext{&index()->artists()}).
    MatchResult match(const ParsedRemote &parsed, const RemoteFile &file, const MatchOptions &options = {}) const;

    // Collection and Inbox records for a local track, best first: tier ≥ likely, or relation OtherVersion; at most
    // `limit`. Lengths are exact on both sides.
    QList<RecordMatch> matchLocal(const LocalTrackQuery &query, qsizetype limit = 8,
                                  const MatchOptions &options = {}) const;

private:
    std::shared_ptr<const MatchIndex> m_index;
};

} // namespace selecta::match
