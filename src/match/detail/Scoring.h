// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Duration.h"
#include "match/Matcher.h"
#include "match/Quality.h"
#include "match/detail/Alignment.h"
#include "match/detail/IndexData.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Internal: candidate retrieval and scoring (note §9, §10, §17.5), shared by Matcher and AlbumMatcher. Pure functions
// over an immutable IndexData; any thread.
namespace selecta::match::detail {

// A remote file or a local track, prepared once for scoring against many records. Views point into the strings this
// object owns (their heap buffers don't move when the object is moved; never copy it).
struct Query
{
    TrackKeys keys;
    FieldView title;
    FieldView artist;
    FieldView album;
    QStringList artistSet;
    QStringList featured;
    bool hasArtist = false;

    bool local = false; // a local track: exact lengths on both sides
    int remoteSec = 0;  // a remote file's LENGTH attribute (0 = absent)
    qint64 localMs = 0; // a local track's length

    // Zones for the token-bag route (§17.5 B), remote files only.
    QStringList zoneKeys; // storage of the normalised zone texts
    AlignTokens body;     // F, one segment per dash-separated part
    AlignTokens brackets; // B
    AlignTokens parent;   // D1
    AlignTokens folders;  // F, D1 and D+ (artist evidence)
    bool hasZones = false;

    bool wholeNameOnly = false;
    bool orderAmbiguous = false;
    int quality = kQualityUnknown;
    bool qualityEstablished = false;
};

Query prepareRemote(const ParsedRemote &parsed, const RemoteFile &file);
Query prepareLocal(const LocalTrackQuery &local);

// The reference-guided token bag (note §17.5 B): a reference's title and artist tokens against a query's zones —
// the title in F (or in D1 at 0.8), the artist in F, D1 and D+; an artist found only inside B is a remixer.
struct ZoneBag
{
    double coverageTitle = 0.0;
    double coverageArtist = 0.0;
    double unexplained = 0.0; // u within the body segments holding the title (§17.4)
    bool artistOnlyInBrackets = false;
    bool fuzzy = false;
    bool fullyExplained = false; // nothing unexplained in the title's segments
};
ZoneBag zoneBag(const Query &query, const AlignTokens &title, const AlignTokens &artist, FieldMatchOptions options);

// Views of one record's fields into the index vocabularies.
struct RecordFields
{
    FieldView title;
    FieldView artist;
    FieldView album;
};
RecordFields recordFields(const IndexData &data, quint32 record);
FieldView recordTitle(const IndexData &data, quint32 record);
FieldView recordArtist(const IndexData &data, quint32 record);
FieldView recordAlbum(const IndexData &data, quint32 record);

// Candidate records for a query (note §10): squashed-key postings, then the rarest word groups (typos through the
// fuzzy token index), best first, at most `limit`. Mixes are never candidates.
QList<quint32> retrieve(const IndexData &data, const Query &query, qsizetype limit);

struct Scored
{
    quint32 record = 0;
    Tier tier = Tier::None;
    Relation relation = Relation::None;
    VersionCompatibility version = VersionCompatibility::Same;
    FieldScores scores;
    bool fuzzy = false;
    QStringList explanation;
};

// Scores one record (note §9 with the §17.5 B token bag as a second opinion). `explain` fills the explanation.
Scored score(const IndexData &data, const Query &query, quint32 record, bool explain);

// Orders scored candidates: tier, then score, then collection before Inbox, then record index.
bool better(const Scored &a, const Scored &b, const IndexData &data);

// The match fields of a scored record.
RecordMatch toRecordMatch(const IndexData &data, const Scored &scored);

} // namespace selecta::match::detail
