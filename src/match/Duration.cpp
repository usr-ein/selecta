// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Duration.h"

#include <algorithm>
#include <cmath>

namespace selecta::match {

namespace {

DurationComparison windowsFor(double referenceSec, double delta)
{
    DurationComparison comparison;
    comparison.known = true;
    comparison.delta = delta;
    comparison.exactWindow = kExactWindowSec;
    comparison.likelyWindow = std::max(kLikelyWindowMinSec, kLikelyWindowFraction * referenceSec);
    comparison.farWindow = std::max(kFarWindowMinSec, kFarWindowFraction * referenceSec);
    comparison.vetoWindow = std::max(kVetoWindowMinSec, kVetoWindowFraction * referenceSec);
    if (delta <= comparison.exactWindow) {
        comparison.score = 1.0;
    } else if (delta <= comparison.likelyWindow) {
        comparison.score =
            1.0 - (0.5 * (delta - comparison.exactWindow) / (comparison.likelyWindow - comparison.exactWindow));
    } else if (delta <= comparison.farWindow) {
        comparison.score = 0.5 * (comparison.farWindow - delta) / (comparison.farWindow - comparison.likelyWindow);
    } else {
        comparison.score = 0.0;
    }
    return comparison;
}

} // namespace

DurationComparison compareRemoteDuration(int remoteSec, qint64 localMs)
{
    if (remoteSec <= 0 || localMs <= 0) {
        return {};
    }
    const double localSec = static_cast<double>(localMs) / 1000.0;
    // Peers truncate (Nicotine+: int(duration)), so the remote value lies in [d − 1, d]: compare with floor(d).
    const double delta = std::abs(static_cast<double>(remoteSec) - std::floor(localSec));
    return windowsFor(localSec, delta);
}

DurationComparison compareDurations(qint64 candidateMs, qint64 referenceMs)
{
    if (candidateMs <= 0 || referenceMs <= 0) {
        return {};
    }
    const double delta = std::abs(static_cast<double>(candidateMs - referenceMs)) / 1000.0;
    return windowsFor(static_cast<double>(referenceMs) / 1000.0, delta);
}

} // namespace selecta::match
