// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtTypes>

#include <algorithm>
#include <chrono>

// The governor's numbers (ADR-006; rationale and sources in docs/RESEARCH-SYNTHESIS.md §3).
//
// Each default has a hard bound next to it. For counts and rates the bound is a ceiling; for waiting times it is a
// floor. Settings may only make a limit stricter (PRD SAFE-2), and the static_asserts keep every default inside its
// bound. Changing a bound needs evidence from M9 and a change to ADR-006.
namespace selecta::governor::limits {

using namespace std::chrono_literals;

// --- Network searches, every source shares one token bucket (D31) --------------------------------------------------
inline constexpr int kSearchBurst = 6;
inline constexpr int kSearchBurstCeiling = 8;
inline constexpr std::chrono::seconds kSearchRefill = 30s; // one more search every 30 s = 2 per minute
inline constexpr std::chrono::seconds kSearchRefillFloor = 30s;
static_assert(kSearchBurst <= kSearchBurstCeiling);
static_assert(kSearchRefill >= kSearchRefillFloor);

inline constexpr std::chrono::minutes kSameQueryQuiet = 10min; // the cache answers a repeat within this window
inline constexpr std::chrono::minutes kSameQueryQuietFloor = 5min;
static_assert(kSameQueryQuiet >= kSameQueryQuietFloor);

inline constexpr int kQueryMinChars = 3;
inline constexpr int kQueryMaxChars = 256;
inline constexpr int kGetTracksReservedSearches = 2; // left in the bucket for searches typed by hand
static_assert(kGetTracksReservedSearches < kSearchBurst);

// --- Collecting results of one search (SRCH-16) ---------------------------------------------------------------------
inline constexpr std::chrono::seconds kResultsIdle = 15s;
inline constexpr std::chrono::seconds kResultsIdleCeiling = 60s;
inline constexpr std::chrono::seconds kResultsTotal = 120s;
inline constexpr int kResultsPeers = 300;
inline constexpr int kResultsPeersCeiling = 1000;
inline constexpr int kResultsFiles = 10'000;
inline constexpr int kResultsFilesCeiling = 50'000;
static_assert(kResultsIdle <= kResultsIdleCeiling && kResultsIdle <= kResultsTotal);
static_assert(kResultsPeers <= kResultsPeersCeiling && kResultsFiles <= kResultsFilesCeiling);

// --- Login and reconnect (NET-4) ------------------------------------------------------------------------------------
inline constexpr std::chrono::seconds kReconnectFirstMin = 10s;
inline constexpr std::chrono::seconds kReconnectFirstMax = 30s;
inline constexpr std::chrono::seconds kReconnectFirstFloor = 5s;
inline constexpr std::chrono::minutes kReconnectCap = 10min;
inline constexpr std::chrono::minutes kReconnectCapFloor = 5min;
inline constexpr std::chrono::minutes kAfterBanWait = 30min; // plus 1–5 min of jitter, then ask the user
inline constexpr std::chrono::minutes kAfterLoginTimeoutWait = 5min;
inline constexpr std::chrono::minutes kAfterLoginTimeoutCap = 60min;
inline constexpr std::chrono::seconds kLoginReplyTimeout = 30s;
inline constexpr int kLoginAttemptsPerHourCeiling = 10;
static_assert(kReconnectFirstMin >= kReconnectFirstFloor && kReconnectFirstMin <= kReconnectFirstMax);
static_assert(kReconnectCap >= kReconnectCapFloor);

// --- Peers ----------------------------------------------------------------------------------------------------------
inline constexpr int kPeerLookupsInFlight = 10;
inline constexpr int kPeerLookupsPerMinute = 30;
inline constexpr int kPeerLookupsPerMinuteCeiling = 60;
inline constexpr std::chrono::seconds kPeerConnectTimeout = 10s;
inline constexpr std::chrono::seconds kPeerIndirectTimeout = 20s;
inline constexpr std::chrono::seconds kPeerIdleTimeout = 60s;
inline constexpr int kSocketsMax = 512;
static_assert(kPeerLookupsPerMinute <= kPeerLookupsPerMinuteCeiling);

// --- Open files (P-022) ---------------------------------------------------------------------------------------------
// Every socket, file and pipe takes one of the process's descriptors, and a macOS app gets 256 unless it asks for more.
// When they run out, sockets fail with "Out of resources" and Qt aborts the next time it starts a thread (a thread's
// event loop needs descriptors of its own). Selecta asks for kWantedOpenFiles at start-up. Peer sockets get the limit
// in force minus a reserve for everything else (the database, logs, files being transferred or converted, worker
// threads, HTTP, child processes), never more than kSocketsMax: the open-file limit can only lower the socket cap.
inline constexpr int kReservedDescriptors = 256;
// Room for kSocketsMax and the reserve, with some to spare (Linux's usual soft limit).
inline constexpr int kWantedOpenFiles = 1024;
static_assert(kSocketsMax + kReservedDescriptors <= kWantedOpenFiles);

// The descriptors kept for everything but peer sockets under a limit of `openFiles`: kReservedDescriptors, or three
// quarters of a limit too low to leave peer sockets a quarter after it.
constexpr qint64 reservedDescriptors(qint64 openFiles)
{
    const qint64 limit = std::max<qint64>(openFiles, 0);
    return limit >= qint64{kReservedDescriptors} * 4 / 3 ? kReservedDescriptors : limit * 3 / 4;
}

// The peer socket cap under a limit of `openFiles` descriptors (the soft RLIMIT_NOFILE): the limit minus the reserve,
// at least 1, at most kSocketsMax.
constexpr int socketCapFor(qint64 openFiles)
{
    return static_cast<int>(std::clamp<qint64>(openFiles - reservedDescriptors(openFiles), 1, kSocketsMax));
}
static_assert(socketCapFor(kWantedOpenFiles) == kSocketsMax);
static_assert(socketCapFor(256) == 64); // macOS's default leaves 192 descriptors for the rest

// --- Downloads (FAIR-2, DL-2) ---------------------------------------------------------------------------------------
inline constexpr int kPeerActiveDownloads = 1;
inline constexpr int kPeerActiveDownloadsCeiling = 2;
inline constexpr int kPeerQueuedFiles = 20;
inline constexpr int kPeerQueuedFilesCeiling = 50;
inline constexpr int kPeerDailyWarnFiles = 20;
inline constexpr qint64 kPeerDailyWarnBytes = 1'000'000'000;
inline constexpr int kPeerDailyStopFiles = 50;
inline constexpr int kPeerDailyStopFilesCeiling = 100;
inline constexpr qint64 kPeerDailyStopBytes = 3'000'000'000;
inline constexpr qint64 kPeerDailyStopBytesCeiling = 6'000'000'000;
inline constexpr int kPeerWeeklyStopFiles = 150;
inline constexpr int kActiveDownloads = 4;
inline constexpr int kActiveDownloadsCeiling = 10;
inline constexpr int kQueuedDownloads = 200;
inline constexpr int kQueuedDownloadsCeiling = 1000;
inline constexpr std::chrono::minutes kPlaceInQueuePoll = 10min; // head of our queue at each peer only
inline constexpr int kDownloadRetries = 3;                       // at 3, 10 and 30 min
inline constexpr int kDownloadRetriesCeiling = 5;
static_assert(kPeerActiveDownloads <= kPeerActiveDownloadsCeiling && kPeerQueuedFiles <= kPeerQueuedFilesCeiling);
static_assert(kPeerDailyWarnFiles <= kPeerDailyStopFiles && kPeerDailyStopFiles <= kPeerDailyStopFilesCeiling);
static_assert(kPeerDailyWarnBytes <= kPeerDailyStopBytes && kPeerDailyStopBytes <= kPeerDailyStopBytesCeiling);
static_assert(kActiveDownloads <= kActiveDownloadsCeiling && kQueuedDownloads <= kQueuedDownloadsCeiling);
static_assert(kDownloadRetries <= kDownloadRetriesCeiling);

// --- Previews from peers (PREV-6, PREV-7) ---------------------------------------------------------------------------
inline constexpr std::chrono::milliseconds kAutoPreviewRest = 600ms;
inline constexpr int kActivePreviews = 1;
inline constexpr int kActivePreviewsCeiling = 2;
inline constexpr int kPreviewsPer10Min = 30;
inline constexpr int kPreviewsPer10MinCeiling = 60;
inline constexpr int kPreviewFinishPercent = 85; // a skipped preview this far along finishes instead of cancelling
static_assert(kActivePreviews <= kActivePreviewsCeiling && kPreviewsPer10Min <= kPreviewsPer10MinCeiling);

inline constexpr std::chrono::seconds kPreviewRefill = 20s;   // 30 per 10 min, refilled evenly
inline constexpr std::chrono::seconds kPeerLookupRefill = 2s; // 30 per minute
static_assert(std::chrono::minutes(10) / kPreviewRefill <= kPreviewsPer10MinCeiling);
static_assert(std::chrono::minutes(1) / kPeerLookupRefill <= kPeerLookupsPerMinuteCeiling);

// --- Browse, user info, folder contents ------------------------------------------------------------------------------
inline constexpr int kBrowseBurst = 3;
inline constexpr std::chrono::seconds kBrowseRefill = 120s; // the note's 100 s would exceed its own 30/hour ceiling
inline constexpr int kBrowsePerHourCeiling = 30;
inline constexpr std::chrono::minutes kBrowseSameUserQuiet = 10min; // a refresh of the same user's shares
inline constexpr std::chrono::minutes kUserInfoPerUser = 10min;
inline constexpr int kUserInfoBurst = 5;
inline constexpr std::chrono::seconds kUserInfoRefill = 120s; // 30 per hour
inline constexpr int kFolderContentsBurst = 5;
inline constexpr int kFolderContentsPer10Min = 20;
inline constexpr std::chrono::seconds kFolderContentsRefill = 30s;
inline constexpr int kPlaceInQueueBurst = 5;
inline constexpr std::chrono::seconds kPlaceInQueueRefill = 30s;
static_assert(std::chrono::hours(1) / kBrowseRefill <= kBrowsePerHourCeiling); // sustained rate
static_assert(std::chrono::minutes(10) / kFolderContentsRefill <= kFolderContentsPer10Min);

// --- Metadata and preview services (meta/): each service's published or conservative limits ------------------------
inline constexpr int kSpotifyBurst = 5;
inline constexpr std::chrono::milliseconds kSpotifyRefill = 250ms; // 4 per second; 429 Retry-After also honoured
inline constexpr int kMusicBrainzBurst = 1;
inline constexpr std::chrono::milliseconds kMusicBrainzRefill = 1000ms; // their policy: 1 per second
inline constexpr int kCoverArtBurst = 4;
inline constexpr std::chrono::milliseconds kCoverArtRefill = 500ms;
inline constexpr int kDeezerBurst = 10;
inline constexpr std::chrono::milliseconds kDeezerRefill = 100ms; // their policy: 50 per 5 seconds
inline constexpr int kYtDlpBurst = 3;
inline constexpr std::chrono::seconds kYtDlpRefill = 10s;
static_assert(kMusicBrainzRefill >= std::chrono::milliseconds(1000));
static_assert(std::chrono::seconds(5) / kDeezerRefill <= 50);

// --- Private messages and room posts (NET-7, NET-10) ----------------------------------------------------------------
inline constexpr std::chrono::seconds kMessageSpacing = 2s;
inline constexpr std::chrono::minutes kSameMessageQuiet = 10min;

// --- Sharing (SHARE-2, SHARE-3) --------------------------------------------------------------------------------------
inline constexpr int kUploadSlots = 3;
inline constexpr int kUploadsPerUser = 1;
inline constexpr int kUploadQueueFilesPerUser = 100;
inline constexpr qint64 kUploadQueueBytesPerUser = qint64{10'000} * 1024 * 1024;
inline constexpr std::chrono::seconds kUploadStartTimeout = 45s;
inline constexpr std::chrono::seconds kUploadIdleTimeout = 60s;
inline constexpr int kAnswerMinChars = 3;
inline constexpr int kAnswerMaxFiles = 300;
inline constexpr int kAnswerMaxFilesCeiling = 500;
inline constexpr int kAnswerQueue = 200;
static_assert(kAnswerMaxFiles <= kAnswerMaxFilesCeiling);

} // namespace selecta::governor::limits
