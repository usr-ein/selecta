// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "governor/Permit.h"

#include <QString>

#include <optional>

namespace selecta::governor {

// Why the governor refused an operation. Each value is explained in the Decision text shown to the user.
enum class Refusal {
    None,
    NetworkPaused,       // the user paused network activity (SAFE-5)
    RateLimited,         // the operation's budget is spent for now; retryAfter says when
    SameQueryRecently,   // the same search ran within 10 minutes: use the cached results
    QueryTooShort,       // fewer than 3 characters after cleaning
    QueryTooLong,        // more than 256 characters
    QueryExcludedPhrase, // the server excludes a phrase in it: peers would return nothing
    PeerActiveLimit,     // already downloading from this peer
    PeerQueueLimit,      // too many files queued at this peer
    PeerDailyLimit,      // this peer's daily stop (files or bytes) is reached
    PeerWeeklyLimit,     // this peer's 7-day stop is reached
    PeerBackoff,         // the peer denied us recently (Banned, File not shared., Pending shutdown.)
    PeerTooSoon,         // the same per-peer request ran too recently
    GlobalLimit,         // too many downloads active or queued overall
    MessageRepeated,     // the same text went to the same recipient within 10 minutes
    LoginNeedsUser,      // after Relogged, a rejection or a ban: only the user can reconnect
    LoginBackoff,        // waiting before the next login attempt
    LiveServerRefused,   // ServerGuard: this build or setting doesn't allow the live network
};

// The governor's answer to one request: either a permit, or a refusal with its reason and, when it's a matter of
// time, how long to wait. A permitted decision may carry a warning (e.g. a peer's daily soft cap).
struct Decision
{
    std::optional<Permit> permit;
    Refusal refusal = Refusal::None;
    base::Clock::Duration retryAfter{0};
    QString explanation;  // human-readable, for the activity log and the UI
    QString warning;      // set on some allowed decisions
    QString cleanedQuery; // Search only: what to send on the network

    bool allowed() const { return permit.has_value(); }
};

QString refusalName(Refusal refusal);
// Stable short names for logs and the activity table ("search", "queue-download"…).
QString operationName(Operation operation);

} // namespace selecta::governor
