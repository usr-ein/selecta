// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"
#include "core/MatchCorrections.h"
#include "core/SearchService.h"
#include "match/MatchIndex.h"
#include "match/Quality.h"
#include "match/ReleaseScorer.h"
#include "match/RemotePathParser.h"
#include "meta/Release.h"

#include <QList>
#include <QMetaType>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace selecta::core {

// What the user already has of a search result (PRD SRCH-11, LIB-3, LIB-4), in badge precedence.
enum class Ownership : quint8 {
    None,
    InCollection,       // the same track is in the collection (exact)
    Upgrade,            // in the collection, and this file's quality is established and better
    LikelyInCollection, // probably in the collection (a fuzzy match)
    InInbox,            // in the Inbox, not filed yet
    Queued,             // in the transfers
    DownloadedThisSession,
    DownloadedBefore,
    OtherVersion, // another version of it is owned (another mix or edit)
};

// "In collection", "Upgrade"… for badges. Thread-safe (pure).
QString ownershipLabel(Ownership ownership);
// Whether the user has this result (the "Already have" filter, the duplicate guard): every state except None, Upgrade
// and OtherVersion.
bool isOwned(Ownership ownership);

// A library record the matcher paired a result with: the best in the collection and in the Inbox (tier ≥ likely), or
// the best other version. Kept so that the user's corrections (PRD LIB-6) can set one aside and fall back to the next.
struct OwnedCandidate
{
    quint64 id = 0; // the collection file or Inbox item (match::LibraryTrack::id)
    match::Source source = match::Source::Collection;
    match::Tier tier = match::Tier::None;
    match::Relation relation = match::Relation::None; // Same, Upgrade or OtherVersion

    friend bool operator==(const OwnedCandidate &, const OwnedCandidate &) = default;
};

// One file of a search, with what Selecta worked out about it.
struct SearchResult
{
    int index = 0; // its position in the search: stable while the search is open
    SearchHit hit;
    QString path;   // the virtual path, decoded
    QString folder; // the path without the file name
    QString name;   // the file name
    match::ParsedRemote parsed;
    match::RemoteQuality quality;         // declared and estimated quality, plausibility (SRCH-6, SRCH-7)
    std::optional<match::ReleaseFit> fit; // against the chosen release (SRCH-18); unset for "Any (name only)"
    Ownership ownership = Ownership::None;
    QString ownershipDetail;   // the owned file, or where the download is now (set on the engine thread)
    QList<quint64> historyIds; // past downloads that are this very file
    quint64 ownedId = 0;       // the collection file or Inbox item behind the ownership (ownedSource says which)
    match::Source ownedSource = match::Source::Collection;
    QList<OwnedCandidate> candidates; // what the matcher paired it with: collection, Inbox, other version
    int peerFilesToday = 0;           // downloads from this peer today (FAIR-1)
    qint64 peerBackoffMs = 0;         // > 0: Selecta holds off this peer after a denial (FAIR-4)

    bool isAudio() const { return parsed.kind == match::FileKind::Audio; }
    int durationSec() const { return static_cast<int>(hit.attributes.duration.value_or(0)); }
    // Whether this file fits the chosen release: its length and name agree, and it's the same version (SRCH-18).
    bool fitsRelease() const;
};

// How long a download from this hit would take to finish, in seconds, as far as the hit tells (PRD SRCH-10): the
// files queued ahead of it (none with a free slot; at least one without), each taken as kTypicalQueuedFileBytes, plus
// the file itself, at the peer's upload speed (kUnknownSpeedBytesPerSecond when the peer didn't say). An estimate to
// rank by, not a promise: peers share their bandwidth over several slots. Pure; any thread.
inline constexpr qint64 kTypicalQueuedFileBytes = 10'000'000; // about a 320 kbps track of four minutes
inline constexpr qint64 kUnknownSpeedBytesPerSecond = 20'000; // pessimistic: a peer that says nothing is slow
double expectedWaitSeconds(const SearchHit &hit);

// The folder part of a virtual path, and its last component, with either separator. Pure.
QString remoteFolderOf(const QString &path);
QString remoteNameOf(const QString &path);
// A peer's path bytes as text: UTF-8 when valid, Latin-1 otherwise. Pure.
QString decodeRemotePath(const QByteArray &raw);
match::RemoteAttributes remoteAttributesOf(const SearchHit &hit);
// The chosen release as the scorer's reference.
match::ReleaseReference referenceOf(const meta::Release &release);
// A result as a download's file (what the transfer and the arrival pipeline record).
FileChoice fileChoiceOf(const SearchResult &result);
// A hit as far as a filter needs it (SearchFilter::hiddenReason() without a release): its path, kind and quality. No
// release fit and no ownership: for Get tracks and the Wishlist, which judge files without the Search view. Pure.
SearchResult filterableResult(const SearchHit &hit);

// Turns hits into results (PRD SRCH-6, SRCH-7, SRCH-11, SRCH-18): each remote folder is parsed once with its siblings,
// quality is assessed from the attributes and the size, files are scored against the release when there is one, and
// audio files are looked up in the collection, the Inbox and the history. `firstIndex` numbers the results.
//
// Threading: pure; call it from a worker. The index is an immutable snapshot.
QList<SearchResult> analyzeHits(const QList<SearchHit> &hits, int firstIndex,
                                const std::shared_ptr<const match::MatchIndex> &index,
                                const std::optional<match::ReleaseReference> &release);
// Scores results against another release, or clears their fit for none. Pure.
void scoreResults(QList<SearchResult> &results, const std::optional<match::ReleaseReference> &release);
// Looks results up again in a new snapshot (Queued and the engine's details are left to the caller). Pure.
void matchResults(QList<SearchResult> &results, const std::shared_ptr<const match::MatchIndex> &index);
// The ownership a result's candidates and download history give it, in badge precedence: a sure collection match,
// a likely one, the Inbox, a past download, another version. `verdictOf` gives the user's verdict on a candidate (PRD
// LIB-6), by the candidate's id and source: Different sets it aside, Same makes it sure (another version included).
// Sets ownership, ownedId and ownedSource; Queued and the engine's details are left to the caller. Pure.
using CandidateVerdict = std::function<std::optional<Verdict>(quint64 id, match::Source source)>;
void settleOwnership(SearchResult &result, const CandidateVerdict &verdictOf = {});

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::SearchResult)
