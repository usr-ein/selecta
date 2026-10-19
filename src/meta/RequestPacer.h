// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "meta/Result.h"

#include <QByteArray>
#include <QString>

#include <chrono>
#include <functional>
#include <optional>

namespace selecta::meta {

// The governor's answer to one permit request.
struct PermitDecision
{
    std::optional<governor::Permit> permit; // set when allowed: consumed by the request it was asked for
    QString refusal;                        // why not, when refused ("Spotify requests: budget spent for now")
    // Refused only for now (a budget refills): meta asks again after this long, if that fits the service's maxWait
    // (ServicePolicy), else fails the request with ErrorCode::Refused. Zero: refused outright (paused, shutting down).
    std::chrono::milliseconds retryAfter{0};
};

// What meta sent, for the activity log and the inspector (PRD SAFE-4, META-7). Never contains a token, secret,
// Client ID, authorisation code or PKCE verifier: request targets are scrubbed before they get here.
struct RequestRecord
{
    Service service = Service::SpotifyApi;
    QByteArray method;  // "GET", "POST", or "RUN" for a yt-dlp process
    QString target;     // host, path and scrubbed query; for yt-dlp the action ("search …")
    int httpStatus = 0; // 0 when there was no HTTP answer
    qint64 bytes = 0;   // body bytes received
    std::chrono::milliseconds elapsed{0};
    QString outcome; // "ok", "HTTP 429, retrying in 5 s", "timeout", "cached"…
};

// The seam between meta and the network governor (ADR-006). core/ implements it with the Governor; tests implement
// it with tests/support/PermitFactory.h.
//
// meta asks for one permit per outbound request: every HTTP request, every redirect hop, every retry and every
// yt-dlp process that talks to YouTube. meta also paces each service itself to its published limits
// (ServicePolicy.h) *before* asking, so the governor's own budgets can only make things slower.
//
// Threading: called only from the thread that owns the meta objects. The implementation must call `grant` exactly
// once per acquire(), on that same thread, and never from inside acquire() itself (queue it), so callers never see
// re-entrant callbacks. It may call `grant` after the requester is gone: meta's callbacks guard against that and drop
// the unused permit.
//
// Ownership: the pacer must outlive every meta object it is given to.
class RequestPacer
{
public:
    using Grant = std::function<void(PermitDecision decision)>;

    RequestPacer() = default;
    RequestPacer(const RequestPacer &) = delete;
    RequestPacer &operator=(const RequestPacer &) = delete;
    RequestPacer(RequestPacer &&) = delete;
    RequestPacer &operator=(RequestPacer &&) = delete;
    virtual ~RequestPacer();

    // Asks for permission to make one request that needs `operation`. A refusal with retryAfter makes meta wait and
    // ask again (governor::Decision::retryAfter maps there); any other refusal fails the request with
    // ErrorCode::Refused and the refusal text.
    virtual void acquire(governor::Operation operation, Grant grant) = 0;

    // One request finished (or was answered from the cache). For the activity log; must not block.
    virtual void report(const RequestRecord &record) = 0;
};

} // namespace selecta::meta
