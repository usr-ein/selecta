// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/ArtistDictionary.h"
#include "match/MatchIndex.h"
#include "match/Normalizer.h"
#include "match/TokenIndex.h"
#include "match/Types.h"

#include <QHash>
#include <QMultiHash>
#include <QString>
#include <QStringList>

#include <vector>

// Internal: the contents of a MatchIndex snapshot (note §10). Immutable after MatchIndex::build(); read concurrently by
// Matcher and AlbumMatcher.
namespace selecta::match::detail {

// What many records share: interned once (versions, extra bracket text, artist sets).
struct RecordMeta
{
    VersionInfo version; // raw and byDisplay are kept for explanations
    QString extraKey;
    QStringList artistSet;
    QStringList featured;
};

// One collection or Inbox track. Text is held as term ids into the index's vocabularies, so a record is small and
// scoring it allocates nothing (FieldView).
struct TrackRecord
{
    quint64 id = 0;
    quint64 albumId = 0;
    qint64 durationMs = 0;
    quint32 titleBegin = 0; // word-term ids in IndexData::tokenPool (terms of IndexData::words)
    quint32 artistBegin = 0;
    quint32 albumBegin = 0; // album-term ids in tokenPool (terms of IndexData::albumWords)
    quint16 titleCount = 0;
    quint16 artistCount = 0;
    quint16 albumCount = 0;
    quint32 titleSquashed = TokenIndex::kNoTerm; // terms of IndexData::squashed
    quint32 artistSquashed = TokenIndex::kNoTerm;
    quint32 albumSquashed = TokenIndex::kNoTerm;
    quint32 meta = 0;  // IndexData::metas
    qint32 album = -1; // IndexData::albums
    qint16 trackNumber = 0;
    qint16 discNumber = 0;
    Source source = Source::Collection;
    quint8 quality = 0; // qualityRank()
    bool mix = false;   // longer than 20 minutes: never matched as a track
};

// One local album, with its duration signature (note §8).
struct AlbumRecord
{
    quint64 id = 0;
    Source source = Source::Collection;
    FieldKey artist;
    FieldKey title;
    QStringList artistSet;
    int year = 0;
    bool variousArtists = false;
    std::vector<quint32> tracks;      // record indexes, in track order when known
    std::vector<int> sortedDurations; // whole seconds, ascending, tracks with a known length
};

struct HistoryRecord
{
    quint64 id = 0;
    quint64 size = 0;
    QString basenameKey; // case-folded NFC basename
    QString peer;
    QString path;
};

struct IndexData
{
    quint64 generation = 0;
    qsizetype dropped = 0;

    TokenIndex words;      // title and artist words → record indexes (with the fuzzy index)
    TokenIndex albumWords; // album-title words → record indexes (with the fuzzy index)
    TokenIndex squashed;   // squashed title, artist and album keys → record indexes (exact only)
    std::vector<quint32> tokenPool;
    std::vector<TrackRecord> records;
    std::vector<RecordMeta> metas;
    QHash<quint32, QString> titleLeet;  // record → leet key of the title, where it differs from the squashed one
    QHash<quint32, QString> artistLeet; // record → leet key of the artist

    std::vector<AlbumRecord> albums;
    QMultiHash<quint32, quint32> albumsBySignature; // signatureKey(track count, band) → album index

    std::vector<HistoryRecord> history;
    QMultiHash<QString, quint32> historyBySizeName; // historySizeNameKey() → history index
    QMultiHash<QString, quint32> historyByPeerPath; // historyPeerPathKey() → history index

    ArtistDictionary artists;
};

// Duration-signature bucket: track count and the shortest track's band of 8 s (neighbouring bands are probed).
inline constexpr int kSignatureBandSec = 8;
inline quint32 signatureKey(int trackCount, int band)
{
    return (static_cast<quint32>(trackCount) << 16U) | (static_cast<quint32>(band) & 0xFFFFU);
}

// The (size, basename) identity of a past download: Nicotine+'s "already downloaded" rule (note §1).
QString basenameKey(QStringView path);
QString historySizeNameKey(quint64 size, QStringView basenameKey);
QString historyPeerPathKey(QStringView peer, QStringView path);

} // namespace selecta::match::detail
