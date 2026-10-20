// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"
#include "core/SearchFilter.h"
#include "core/SearchService.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace selecta::core {

// What finding a line came to.
struct RankedChoices
{
    QList<FileChoice> files; // best first: one per peer, at most kMaxChoices
    int candidates = 0;      // files that passed the quality filter
    int considered = 0;      // audio files in the results
};

// Ranks a search's files against a line (PRD BATCH-5): files that pass the list's filter preset (BATCH-2, SRCH-8),
// scored against the release with
// match's ReleaseScorer (length first, then name and version), then quality, then availability as a tie-breaker,
// keeping each peer's best file. Without a release, the line's artist, title and length hint stand in. Pure; any
// thread.
class FileRanking
{
public:
    static constexpr int kMaxChoices = 5;

    static RankedChoices rank(const GetLine &line, const SearchOutcome &outcome, const SearchFilter &filter);
    // The words to search for a line (match's QueryPlanner), or an empty string when nothing selective is left.
    static QString query(const GetLine &line, const QStringList &excludedPhrases, QString *why = nullptr);
};

} // namespace selecta::core
