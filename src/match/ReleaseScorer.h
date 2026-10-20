// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/RemotePathParser.h"
#include "match/Types.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace selecta::match {

// Release fit (note §17.5 B, PRD D23/SRCH-18: length first).
inline constexpr double kFitWeightDuration = 0.35;
inline constexpr double kFitWeightTitle = 0.40;
inline constexpr double kFitWeightArtist = 0.25;
inline constexpr double kOnlyThisReleaseMinFit = 0.8; // "Only this release" keeps fit ≥ 0.8 with s_dur = 1
inline constexpr double kArtistOnlyInBracketsFactor = 0.3;
inline constexpr double kUnexplainedFactor = 0.5;       // u > 0.3: another title containing ours
inline constexpr double kParentFolderTitleFactor = 0.8; // the title found in the parent folder ("Title\01.flac")

// The chosen release, as a metadata service lists it (SRCH-17; the meta library's Release maps onto this).
struct ReleaseReference
{
    QStringList artists;   // primary artists as listed ("Bicep"; several for a collaboration)
    QString title;         // the track title as listed, version text included ("Glue (Bicep Remix)")
    QString album;         // optional
    qint64 durationMs = 0; // 0 = unknown
};

struct ReleaseFit
{
    double fit = 0.0;  // (0.35·s_dur + 0.40·cov_T + 0.25·cov_A) / (weights of the available terms) × multipliers
    bool fits = false; // "Only this release" (SRCH-18): fit ≥ 0.8, and s_dur = 1 unless lengthUnknown
    bool lengthUnknown = false;     // a length is missing on either side: the fit was computed on names alone; say so
    std::optional<double> duration; // s_dur
    double titleCoverage = 0.0;     // cov_T: the release's title tokens found in the filename body (or parent × 0.8)
    double artistCoverage = 0.0;    // cov_A: the release's artist tokens found in the body or the folders
    double unexplained = 0.0;       // u within the body segments that hold the title
    VersionCompatibility version = VersionCompatibility::Same;
    bool artistOnlyInBrackets = false; // the release's artist appears only as a remixer: × 0.3 (T73)
    bool fuzzy = false;                // a typo was tolerated
    QStringList explanation;           // when requested
};

// One search result to rank against the release ("Best match", SRCH-18, BATCH-5).
struct RankedFile
{
    ReleaseFit fit;        // ReleaseScorer::score() of the file
    int qualityRank = 0;   // assessRemoteQuality().rank, or the preset's own quality order (higher is better)
    bool freeSlot = false; // the peer has a free upload slot
    int queueLength = 0;   // the peer's queue length (shorter is better)
    QString peer;          // for a deterministic order
    QString path;
};

// Scores remote files against one release (note §17.5 B), position-independently: zones F (filename body), B
// (its brackets), D1 (parent folder) and D+ (higher folders); the artist found only inside B is a remixer, which counts
// as version evidence, never as artist evidence; a remix or edit missing from the release is a version conflict (× 0).
// Used for search with a chosen release, Get tracks lines (BATCH-5), the wishlist and META-2.
//
// Threading: immutable after construction; score() may be called concurrently from any thread. Copies share the
// prepared reference. Failure modes: none.
class ReleaseScorer final
{
public:
    explicit ReleaseScorer(const ReleaseReference &reference);

    const ReleaseReference &reference() const;

    // `durationSec` is the result's LENGTH attribute (0 = absent).
    ReleaseFit score(const ParsedRemote &parsed, int durationSec, bool explain = false) const;

    // Identity order for "Best match" (SRCH-18): fits first, then length (s_dur), then artist, title and version
    // evidence; never names alone ahead of length. Negative when `a` ranks before `b`, 0 when equal. Quality,
    // availability and fairness are the caller's next keys.
    static int compareIdentity(const ReleaseFit &a, const ReleaseFit &b);

    // Indexes of `files` in "Best match" order (note §9.1): identity first (compareIdentity: fits, length, artist,
    // title, version), then quality, then availability (a free slot, then a shorter queue) as a tie-breaker only,
    // then peer and path so the order is deterministic. Availability never outranks a better fit or better quality.
    // Peer fairness (fewest downloads from a peer) is the caller's to apply among equals.
    static QList<qsizetype> rank(const QList<RankedFile> &files);

private:
    struct Prepared;
    std::shared_ptr<const Prepared> m_prepared;
};

} // namespace selecta::match
