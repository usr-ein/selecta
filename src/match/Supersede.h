// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace selecta::match {

// Supersede guards (note §15, PRD DL-7/DL-8, ADR-008).
inline constexpr int kCircuitBreakerMaxTrash = 10;      // more trashes than this in one album event: ask first
inline constexpr double kCircuitBreakerMaxShare = 0.30; // or more than this share of the Inbox singles

enum class AlbumDownloadState : quint8 {
    InProgress,
    CompleteVerified, // every expected audio file completed and passed verification: the only state that may trash
    Partial,
    Failed,
    Cancelled,
};

enum class SinglePlace : quint8 {
    Inbox,
    Collection, // filed singles are never touched: flag only (DL-8)
};

// A local file's tags and audio facts (the scanner's values).
struct LocalTrackFacts
{
    QString artist;
    QString title; // with its version text
    qint64 durationMs = 0;
    AudioQuality quality;
};

struct SupersedeAlbumTrack
{
    quint64 id = 0;
    LocalTrackFacts track;
};

struct SupersedeSingle
{
    quint64 id = 0;
    LocalTrackFacts track;
    SinglePlace place = SinglePlace::Inbox;
    bool busy = false;                 // playing, converting, filing or pinned by the user: defer
    bool changedSinceIndexing = false; // inode, size or mtime differ from the index (core re-stats right before)
    bool arrivedAfterAlbum = false;    // completed after its album was present (the user overrode DL-11): flag only
};

enum class SupersedeAction : quint8 {
    None,    // leave it alone
    Flag,    // show the pair for review ("also on album X")
    Trash,   // move the single to the macOS Trash with Undo (core's FileOps.trashFromInbox)
    Defer,   // the single is busy: decide again when it is idle
    Recheck, // the file changed since indexing: abort, rescan, decide again
};

enum class SupersedeReason : quint8 {
    ExactMatch,          // every guard holds: trash
    LikelyMatch,         // tier likely: flag
    AlbumIncomplete,     // the album is not complete and verified: flag only (T39)
    CollectionSingle,    // the single is filed: flag only (T38)
    SingleBetterQuality, // the single is better than the album copy: flag (T37)
    QualityUnknown,      // a quality rank is unknown, so "at least as good" can't be shown: flag
    ArrivedAfterAlbum,   // flag only
    SingleBusy,          // defer
    SingleChanged,       // recheck (T41)
    VersionDiffers,      // another version of the same title: no supersede (T40)
};

struct SupersedePair
{
    quint64 albumTrackId = 0;
    quint64 singleId = 0;
    Tier tier = Tier::None;
    SupersedeAction action = SupersedeAction::None;
    SupersedeReason reason = SupersedeReason::VersionDiffers;
    QStringList explanation;
};

struct SupersedeDecision
{
    QList<SupersedePair> pairs;     // one per single that matched an album track (best album track), input order
    int trashCount = 0;             // pairs whose action is Trash
    bool needsConfirmation = false; // circuit breaker: trashCount > 10 or > 30 % of the Inbox singles (T42)
    QString confirmationReason;     // why, for the confirmation sheet
};

// The supersede decision table (note §15), pure: core supplies the facts (album state, the single's place, busy and
// changed flags from its own re-stat) and performs the file operations. The matching is local against local, so it is
// stricter than for search results: exact needs equal title keys, equal artist sets, the same version and lengths
// within 2 s; likely (the §9 rules with exact lengths) only flags.
//
// Order of the guards for an exact pair: the single changed → Recheck; busy → Defer; filed → Flag; arrived after the
// album → Flag; album not complete and verified → Flag; a quality rank unknown → Flag; single better → Flag; else
// Trash. Then the circuit breaker looks at all Trash pairs of the event: they stay Trash, and needsConfirmation tells
// core to ask the user first.
//
// Threading: pure static functions; any thread. Failure modes: none.
class SupersedeDecider final
{
public:
    // Every single against every album track of one completed album event. `inboxSingleCount` is the number of
    // singles in the Inbox now (the circuit breaker's 30 % rule); singles that match no album track at tier ≥ likely
    // (or as another version) are left out.
    static SupersedeDecision decide(AlbumDownloadState state, const QList<SupersedeAlbumTrack> &albumTracks,
                                    const QList<SupersedeSingle> &singles, int inboxSingleCount);

    // One pair, without the circuit breaker. Tier None with reason VersionDiffers when the versions differ; tier
    // None and action None when nothing matches.
    static SupersedePair decidePair(AlbumDownloadState state, const SupersedeAlbumTrack &albumTrack,
                                    const SupersedeSingle &single);
};

} // namespace selecta::match
