// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"
#include "core/SearchFilter.h"
#include "core/SearchService.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace selecta::core {

// What finding a line came to.
struct RankedChoices
{
    QList<FileChoice> files; // best first: one per peer, at most kMaxChoices
    int candidates = 0;      // files that passed the quality filter
    int considered = 0;      // audio files in the results
    // When no file passed: the files of the track (they fit the release) that the preset turned down, best first,
    // one per peer, at most kMaxChoices, each with its shortfall ("Get anyway"). Empty whenever `files` isn't.
    QList<FileChoice> nearMisses;
    int belowQuality = 0; // files of the track the preset turned down, every peer's (nearMisses keeps the best few)
};

// Ranks a search's files against a line (PRD BATCH-5): files that pass the list's filter preset (BATCH-2, SRCH-8),
// scored against the release with match's ReleaseScorer (length first, then name and version), then quality, then
// availability as a tie-breaker, keeping each peer's best file. Without a release, the line's artist, title and length
// hint stand in. When no file passes, the files the preset turned down that fit the release are ranked the same way:
// the near misses, each saying why it fell short. Pure; any thread.
class FileRanking
{
public:
    static constexpr int kMaxChoices = 5;

    // `peerLoad` says how many files were taken from a peer today (FAIR-5); empty: nothing known.
    using PeerLoad = std::function<int(const QString &peer)>;
    static RankedChoices rank(const GetLine &line, const SearchOutcome &outcome, const SearchFilter &filter,
                              const PeerLoad &peerLoad = {});
    // The words to search for a line (match's QueryPlanner), or an empty string when nothing selective is left.
    static QString query(const GetLine &line, const QStringList &excludedPhrases, QString *why = nullptr);
    // Why `filter` turns `result` down for `reason`, in a few words ("under 320 kbps", "no free slot"); empty for
    // HiddenReason::None and the reasons a ranking never meets (not audio, not this release).
    static QString shortfall(HiddenReason reason, const SearchFilter &filter, const SearchResult &result);
};

} // namespace selecta::core
