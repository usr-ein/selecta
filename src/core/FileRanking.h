// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"
#include "core/SearchService.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace selecta::core {

// The quality a Get tracks list wants (PRD BATCH-2). Everything arrives as MP3 320 (D29), so by default only files
// that can make a real MP3 320 qualify: lossless sources and MP3s at 320 kbps.
enum class QualityWanted {
    Mp3_320OrLossless,
    LosslessOnly,
    Any,
};

// What finding a line came to.
struct RankedChoices
{
    QList<FileChoice> files; // best first: one per peer, at most kMaxChoices
    int candidates = 0;      // files that passed the quality filter
    int considered = 0;      // audio files in the results
};

// Ranks a search's files against a line (PRD BATCH-5): files of the wanted quality, scored against the release with
// match's ReleaseScorer (length first, then name and version), then quality, then availability as a tie-breaker,
// keeping each peer's best file. Without a release, the line's artist, title and length hint stand in. Pure; any
// thread.
class FileRanking
{
public:
    static constexpr int kMaxChoices = 5;

    static RankedChoices rank(const GetLine &line, const SearchOutcome &outcome, QualityWanted wanted);
    // The words to search for a line (match's QueryPlanner), or an empty string when nothing selective is left.
    static QString query(const GetLine &line, const QStringList &excludedPhrases, QString *why = nullptr);
};

} // namespace selecta::core
