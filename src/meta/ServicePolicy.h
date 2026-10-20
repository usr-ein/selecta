// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Result.h"

#include <QtTypes>

#include <chrono>

// How meta paces and bounds each service, from the services' published limits
// (references/metadata-and-preview-services.md §14.3). These are compile-time constants, like the governor's
// (SAFE-2): meta never goes faster, and the governor's permits (RequestPacer) can only make it slower.
namespace selecta::meta {

using namespace std::chrono_literals;

struct ServicePolicy
{
    // Starts of requests (every retry and redirect hop counts) are spaced by at least minSpacing, and at most
    // windowCount start in any sliding window of `window`.
    std::chrono::milliseconds minSpacing{0};
    int windowCount = 1;
    std::chrono::milliseconds window{0};
    int maxInFlight = 1;

    std::chrono::milliseconds requestTimeout{0}; // per attempt, from send to the last byte
    qint64 maxBodyBytes = 0;                     // larger answers are aborted (ErrorCode::TooLarge)
    int maxRedirects = 0;                        // followed by hand, each hop paced and permitted

    // Retries after 429, 503 or a service-specific "slow down": at most maxRetries more attempts. The wait is the
    // service's Retry-After when it sends one, else firstBackoff doubling up to maxBackoff.
    int maxRetries = 0;
    std::chrono::milliseconds firstBackoff{0};
    std::chrono::milliseconds maxBackoff{0};
    // A request waits for a back-off at most this long; a longer one fails it at once with ErrorCode::RateLimited
    // (and retryAfter), and later requests fail the same way until the back-off ends. Nothing hangs for hours.
    std::chrono::milliseconds maxWait{0};
};

inline constexpr qint64 kMiB = qint64{1024} * 1024;

// api.spotify.com: no published number (rolling 30-s window, development mode lower); 1 request/s with bursts of 5.
inline constexpr ServicePolicy kSpotifyApiPolicy{200ms, 5, 5s, 2, 15s, 8 * kMiB, 3, 2, 5s, 60s, 30s};
// accounts.spotify.com: sign-in and refreshes only, on demand.
inline constexpr ServicePolicy kSpotifyAccountsPolicy{2s, 6, 60s, 1, 15s, 1 * kMiB, 0, 1, 10s, 60s, 30s};
// Cover images on the Spotify and Deezer CDNs.
inline constexpr ServicePolicy kCoverImagesPolicy{500ms, 10, 10s, 2, 30s, 20 * kMiB, 3, 1, 5s, 30s, 30s};
// api.deezer.com: "50 requests / 5 seconds" per IP (second-hand); meta uses 2/s. Quota errors (code 4): 5 s ×2.
inline constexpr ServicePolicy kDeezerPolicy{500ms, 10, 5s, 2, 15s, 4 * kMiB, 3, 2, 5s, 300s, 30s};
// musicbrainz.org: 1 request/s on average per IP, and 503 above it; start 1.1 s apart. 503: 2 s ×2 up to 60 s,
// at most 3 tries.
inline constexpr ServicePolicy kMusicBrainzPolicy{1100ms, 1, 1100ms, 1, 20s, 8 * kMiB, 3, 2, 2s, 60s, 60s};
// coverartarchive.org and its archive.org redirects: no stated limit, 503 documented; 1/s with small bursts.
inline constexpr ServicePolicy kCoverArtArchivePolicy{500ms, 5, 5s, 2, 30s, 20 * kMiB, 5, 2, 2s, 60s, 60s};

// The published limit for `service`. YtDlp has its own numbers (below); its entry here only carries the launch pacing.
const ServicePolicy &policyFor(Service service);

// yt-dlp (D26, PREV-11): one process at a time, launches ≥ 2 s apart, ≤ 30 launches in 10 minutes, and no yt-dlp at
// all for 60 minutes after YouTube rate-limits or asks for a bot check.
inline constexpr ServicePolicy kYtDlpPolicy{2s, 30, 600s, 1, 120s, 4 * kMiB, 0, 0, 0ms, 0ms, 15s};
inline constexpr std::chrono::minutes kYtDlpPauseAfterRateLimit = 60min;

// Cache lifetimes (MetaCache). Search answers change; catalogue entities and MusicBrainz data change slowly.
inline constexpr std::chrono::hours kSearchTtl = 24h;
inline constexpr std::chrono::hours kEntityTtl = 24h * 30; // track, album, MusicBrainz lookups
inline constexpr std::chrono::hours kNotFoundTtl = 24h;    // "nothing there" answers (ISRC unknown to MusicBrainz)
inline constexpr std::chrono::hours kCoverTtl = 24h * 30;  // cover bytes in the cover cache folder

// Hard bounds on data read from the network or from files.
inline constexpr qint64 kMaxCoverBytes = 20 * kMiB; // 20 MiB
inline constexpr int kMaxCoverPixels = 10'000;      // per side, checked from the header before decoding

static_assert(kMusicBrainzPolicy.minSpacing >= 1s, "MusicBrainz allows 1 request per second on average");
static_assert(qint64{kDeezerPolicy.windowCount} * 1000 / kDeezerPolicy.window.count() <= 10,
              "Deezer allows 50 requests per 5 s (10/s)");
static_assert(kYtDlpPolicy.maxInFlight == 1, "one yt-dlp process at a time (PREV-11)");
static_assert(kYtDlpPolicy.minSpacing >= 2s && kYtDlpPolicy.windowCount <= 30 && kYtDlpPolicy.window >= 600s);
static_assert(kCoverImagesPolicy.maxBodyBytes == kMaxCoverBytes &&
              kCoverArtArchivePolicy.maxBodyBytes == kMaxCoverBytes);

} // namespace selecta::meta
