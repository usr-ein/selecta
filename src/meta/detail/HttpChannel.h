// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Priority.h"
#include "meta/Result.h"
#include "meta/ServicePolicy.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class QNetworkAccessManager;
class QNetworkReply;

namespace selecta::meta {
class MetaCache;
class RequestPacer;
struct PermitDecision;
} // namespace selecta::meta

namespace selecta::meta::detail {

class ServiceGate;

// "Selecta/<version> ( https://github.com/usr-ein/selecta )": the User-Agent of every request (MusicBrainz requires
// an application name, version and contact URL; the others get the same, no personal data).
QByteArray userAgent();

struct HttpRequest
{
    QByteArray method = QByteArrayLiteral("GET");
    QUrl url;
    QList<std::pair<QByteArray, QByteArray>> headers; // never logged
    QByteArray body;                                  // POST: never logged
    QString cacheKey;                                 // GET: answered from / stored in the MetaCache when set
    std::chrono::seconds cacheTtl{0};                 // lifetime of a stored 200 answer
    std::chrono::seconds notFoundTtl{0};              // > 0: a 404 is remembered this long too
    QString logTarget;                                // what the activity record shows; default: the scrubbed URL
    Priority priority = Priority::Interactive;        // the gate lane every attempt of this request waits in
};

struct HttpResponse
{
    int status = 0;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;
    QUrl url;
    bool fromCache = false;

    // The first header with this name (case-insensitive), or empty.
    QByteArray header(QByteArrayView name) const;
};

// What to do with an answer. Clients override the default for service-specific signals (Spotify's
// QUOTA_EXCEEDED, Deezer's error code 4 inside an HTTP 200).
struct RetryAdvice
{
    enum class Kind : quint8 {
        Accept, // hand the answer to the client
        // Back the whole service off for `delay` (else the policy's back-off) and try again, within the policy's
        // retries and maxWait.
        Retry,
        Fail, // fail with `error`
    };
    Kind kind = Kind::Accept;
    std::optional<std::chrono::milliseconds> delay;
    bool blockService = false;      // Fail: also hold every request to the service for `delay`
    std::optional<MetaError> error; // Fail; for Retry, the error reported if the retries run out
};
using Inspector = std::function<RetryAdvice(const HttpResponse &response)>;

// 429 and 503 → Retry, blocking the service, after Retry-After if given; 500, 502, 504 → Retry; else Accept.
RetryAdvice defaultInspection(Service service, const HttpResponse &response);

// A Retry-After value: delta seconds or an HTTP date (relative to `now`), bounded to 0…48 h; nullopt if unreadable.
std::optional<std::chrono::milliseconds> parseRetryAfter(QByteArrayView value, const QDateTime &now);

// The URL as safe to log: scheme, host, path and query, minus anything secret (code, state, tokens, client id…).
QString scrubbedTarget(const QUrl &url);

// The error for an accepted answer that isn't 2xx: 400 InvalidArgument, 401 SignInRequired, 403 Forbidden,
// 404 NotFound, 429 RateLimited, 5xx ServerError, other BadResponse.
MetaError statusError(Service service, const HttpResponse &response);

// All HTTP traffic to one service: meta's own pacing (ServiceGate, from the service's ServicePolicy), one governor
// permit per attempt (RequestPacer), a per-attempt timeout, bounded bodies, redirects followed by hand (HTTPS only,
// each hop paced and permitted, Authorization dropped on a host change), retries with back-off, the MetaCache, and an
// activity record per attempt.
//
// `done` runs exactly once per send() — never inside send() — unless the request is cancelled or the channel is
// destroyed first. Threading: the owner's thread; the network manager must live there too. Ownership: the network
// manager, pacer and cache must outlive the channel.
class HttpChannel final : public QObject
{
    Q_OBJECT

public:
    using RequestId = quint64;
    using Done = std::function<void(Result<HttpResponse> result)>;

    HttpChannel(Service service, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
                const base::Clock &clock, QObject *parent = nullptr);
    ~HttpChannel() override;
    HttpChannel(const HttpChannel &) = delete;
    HttpChannel &operator=(const HttpChannel &) = delete;
    HttpChannel(HttpChannel &&) = delete;
    HttpChannel &operator=(HttpChannel &&) = delete;

    Service service() const { return m_service; }
    const ServicePolicy &policy() const { return m_policy; }
    MetaCache *cache() const { return m_cache; }

    void setInspector(Inspector inspector) { m_inspector = std::move(inspector); }
    // The per-attempt timeout; the policy's by default. Tests use a short one (a timeout isn't a governor limit).
    void setRequestTimeout(std::chrono::milliseconds timeout) { m_timeout = timeout; }

    RequestId send(HttpRequest request, Done done);
    // Drops a request: it never calls back. Unknown or finished IDs are ignored.
    void cancel(RequestId id);

    // Stops every request to the service for `delay` (e.g. Spotify's quota). A longer wait than the policy's maxWait
    // makes new requests fail at once with RateLimited.
    void blockFor(std::chrono::milliseconds delay);
    base::Clock::Duration blockRemaining() const;

    ServiceGate &gate() { return *m_gate; }

private:
    struct Op;
    enum class Abort : quint8 {
        None,
        Timeout,
        TooLarge,
    };

    void enqueueAttempt(const std::shared_ptr<Op> &op, bool front);
    void onGateReady(RequestId id, quint64 ticket);
    void onPermit(RequestId id, quint64 ticket, PermitDecision &decision);
    void startReply(const std::shared_ptr<Op> &op);
    void onReadyRead(RequestId id);
    void onFinished(RequestId id, QNetworkReply *reply);
    void followRedirect(const std::shared_ptr<Op> &op, const HttpResponse &response, std::chrono::milliseconds elapsed);
    void handleAnswer(const std::shared_ptr<Op> &op, HttpResponse response, std::chrono::milliseconds elapsed);
    void abortReply(RequestId id, Abort reason);
    void complete(const std::shared_ptr<Op> &op, Result<HttpResponse> result);
    void failLater(Done done, MetaError error);
    void report(const HttpRequest &request, int status, qint64 bytes, std::chrono::milliseconds elapsed,
                const QString &outcome);
    std::optional<MetaError> blockedError() const;
    std::chrono::milliseconds backoff(int attempt) const;
    MetaError error(ErrorCode code, QString message, int httpStatus = 0) const;

    const Service m_service;
    const ServicePolicy &m_policy;
    QPointer<QNetworkAccessManager> m_network;
    RequestPacer &m_pacer;
    MetaCache *m_cache;
    const base::Clock &m_clock;
    std::unique_ptr<ServiceGate> m_gate;
    Inspector m_inspector;
    std::chrono::milliseconds m_timeout;
    QHash<RequestId, std::shared_ptr<Op>> m_ops;
    RequestId m_nextId = 1;
};

} // namespace selecta::meta::detail
