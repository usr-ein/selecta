// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Matcher.h"

#include <QList>
#include <QStringList>

#include <memory>

namespace selecta::match {

// Album ownership (note §8; Lidarr's 0.20/0.40 thresholds as a starting point).
inline constexpr double kAlbumInCollectionDistance = 0.20;
inline constexpr double kAlbumInCollectionCoverage = 0.90; // share of remote tracks mapped at tier ≥ likely
inline constexpr int kSignatureMinTracks = 5;              // a duration signature alone needs ≥ 5 tracks
inline constexpr double kSignatureWindowSec = 3.0;         // sorted lengths agree within ±3 s each (Sockseek)
inline constexpr qsizetype kMaxAlbumTracks = 100;          // remote audio files considered per folder

// One audio file of a remote folder (disc sub-folders merged by the caller: pass all files of the album).
struct RemoteAlbumFile
{
    ParsedRemote parsed;
    RemoteFile file;
};

struct AlbumTrackMapping
{
    qsizetype remoteIndex = -1; // index into the files passed to match()
    quint64 trackId = 0;        // LibraryTrack::id of the album track it maps to
    Tier tier = Tier::None;
    double distance = 1.0; // track distance used by the assignment (lower is better)
};

struct AlbumMatch
{
    bool found = false;
    quint64 albumId = 0; // LibraryAlbum::id
    Source source = Source::Collection;
    Tier tier = Tier::None;     // exact: in collection with every remote track mapped exact; likely: in collection,
                                // or a duration-signature match (T48)
    double distance = 1.0;      // album distance in [0, 1] (note §8 step 4)
    bool inCollection = false;  // distance ≤ 0.20 and ≥ 90 % of the remote tracks mapped at tier ≥ likely
    bool signatureOnly = false; // matched by duration signature without name evidence
    int localTracks = 0;
    int mappedLikely = 0; // remote tracks mapped to this album's tracks at tier ≥ likely
    QList<AlbumTrackMapping> mapping;
};

struct AlbumMatchResult
{
    AlbumMatch best;         // the best local album, if any
    int remoteTracks = 0;    // audio files considered
    int owned = 0;           // remote tracks owned anywhere at tier ≥ likely: "owned 7/12"
    int ownedAsSingles = 0;  // of those, owned outside the best album: "(3 as singles)"
    QList<Tier> trackTiers;  // per file passed in, its best ownership tier anywhere (None for non-audio files)
    QStringList explanation; // when MatchOptions::explain
};

// Remote folder ↔ local album (note §8): candidates by album key and title tokens and by duration signature (track
// count and shortest-length band, ±3 s per sorted length, subsets of a few tracks); remote tracks are assigned to the
// album's tracks with the Hungarian algorithm (cost: track distance, + 0.1 when track numbers differ); the album
// distance weighs artist 3, title 3, year 1 (±1 free), mean track distance 2, missing tracks 0.9 and extra tracks 0.6
// each; coverage counts every remote track owned anywhere.
//
// Threading: immutable; match() may be called concurrently from any thread. Failure modes: none; at most
// kMaxAlbumTracks audio files are considered.
class AlbumMatcher final
{
public:
    explicit AlbumMatcher(std::shared_ptr<const MatchIndex> index);

    AlbumMatchResult match(const QList<RemoteAlbumFile> &files, const MatchOptions &options = {}) const;

private:
    std::shared_ptr<const MatchIndex> m_index;
};

} // namespace selecta::match
