// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtTypes>

namespace selecta::match {

// Length windows (note §7, PRD D23/SRCH-18, ADR-008). Length is the main identity signal before a download.
inline constexpr double kExactWindowSec = 3.0;     // "same recording" window, also used by the release fit
inline constexpr double kLikelyWindowMinSec = 6.0; // t_likely = max(6 s, 3 %)
inline constexpr double kLikelyWindowFraction = 0.03;
inline constexpr double kFarWindowMinSec = 15.0; // t_far = max(15 s, 8 %)
inline constexpr double kFarWindowFraction = 0.08;
inline constexpr double kVetoWindowMinSec = 60.0; // veto beyond max(60 s, 25 %)
inline constexpr double kVetoWindowFraction = 0.25;
inline constexpr int kMixMinSec = 20 * 60;          // longer files are mixes or sets: no track matching
inline constexpr double kLocalExactWindowSec = 2.0; // supersede: local against local (note §15)

// The comparison of two lengths and the duration similarity s_dur (note §7):
//   s_dur = 1                                    if Δ ≤ t_exact
//         = 1 − 0.5·(Δ − t_exact)/(t_likely − t_exact)   if Δ ≤ t_likely
//         = 0.5·(t_far − Δ)/(t_far − t_likely)           if Δ ≤ t_far
//         = 0                                    otherwise
// The windows scale with the local (reference) length.
struct DurationComparison
{
    bool known = false; // false when either length is missing; then nothing else is meaningful
    double delta = 0.0; // |Δ| in seconds
    double exactWindow = 0.0;
    double likelyWindow = 0.0;
    double farWindow = 0.0;
    double vetoWindow = 0.0;
    double score = 0.0; // s_dur in [0, 1]

    bool withinExact() const { return known && delta <= exactWindow; }
    bool withinLikely() const { return known && delta <= likelyWindow; }
    bool beyondFar() const { return known && delta > farWindow; }
    bool vetoed() const { return known && delta > vetoWindow; }
};

// A remote length (whole seconds, as peers report it: usually truncated) against a local one in milliseconds:
// Δ = |remote − floor(local)|, so truncation costs nothing (note §7). A remote length ≤ 0 or a local one ≤ 0 is
// missing. Pure; any thread.
DurationComparison compareRemoteDuration(int remoteSec, qint64 localMs);

// Two exact lengths in milliseconds (a release or a local file against another local file): Δ = |a − b| in seconds,
// windows scaled by `referenceMs`. Pure; any thread.
DurationComparison compareDurations(qint64 candidateMs, qint64 referenceMs);

// Mixes and DJ sets: track matching is skipped for them (note §7).
inline bool isMixLength(int seconds)
{
    return seconds > kMixMinSec;
}

} // namespace selecta::match
