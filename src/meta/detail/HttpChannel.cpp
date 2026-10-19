// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/HttpChannel.h"

#include "base/Version.h"
#include "meta/MetaCache.h"
#include "meta/RequestPacer.h"
#include "meta/detail/ServiceGate.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
Q_LOGGING_CATEGORY(lcMetaHttp, "selecta.meta.http")
} // namespace

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::meta::detail {

namespace {

constexpr auto kMaxRetryAfter = std::chrono::hours(48);
constexpr QLatin1StringView kNotFoundSuffix("#404"); // cache key suffix of a remembered 404

std::chrono::seconds roundUpSeconds(std::chrono::milliseconds ms)
{
    return std::chrono::ceil<std::chrono::seconds>(std::max(ms, 0ms));
}

QString describeWait(std::chrono::milliseconds wait)
{
    return u"%1 s"_s.arg(roundUpSeconds(wait).count());
}

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

ErrorCode networkErrorCode(QNetworkReply::NetworkError error)
{
    return error == QNetworkReply::TimeoutError ? ErrorCode::Timeout : ErrorCode::Network;
}

} // namespace

QByteArray userAgent()
{
    return QByteArrayLiteral("Selecta/") + QByteArray(base::kVersion.data(), base::kVersion.size()) +
           QByteArrayLiteral(" ( https://github.com/usr-ein/selecta )");
}

QByteArray HttpResponse::header(QByteArrayView name) const
{
    for (const auto &[key, value] : headers) {
        if (key.compare(name, Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return {};
}

std::optional<std::chrono::milliseconds> parseRetryAfter(QByteArrayView value, const QDateTime &now)
{
    const QByteArray trimmed = value.trimmed().toByteArray();
    if (trimmed.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const qlonglong seconds = trimmed.toLongLong(&ok);
    std::chrono::milliseconds wait{};
    if (ok) {
        if (seconds < 0) {
            return std::nullopt;
        }
        wait = std::chrono::seconds(std::min<qlonglong>(seconds, kMaxRetryAfter.count() * 3600));
    } else {
        // An HTTP date (RFC 9110 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"). Qt's RFC 2822 reader wants a numeric
        // zone, so GMT becomes +0000.
        QString date = QString::fromLatin1(trimmed);
        if (date.endsWith(u" GMT"_s) || date.endsWith(u" UTC"_s)) {
            date.chop(3);
            date += u"+0000"_s;
        }
        const QDateTime when = QDateTime::fromString(date, Qt::RFC2822Date);
        if (!when.isValid()) {
            return std::nullopt;
        }
        wait = std::chrono::milliseconds(std::max<qint64>(0, now.msecsTo(when)));
    }
    return std::min<std::chrono::milliseconds>(wait, kMaxRetryAfter);
}

QString scrubbedTarget(const QUrl &url)
{
    static const std::array<QString, 9> kSecretKeys = {u"code"_s,         u"code_verifier"_s, u"refresh_token"_s,
                                                       u"access_token"_s, u"client_id"_s,     u"client_secret"_s,
                                                       u"state"_s,        u"token"_s,         u"code_challenge"_s};
    QUrlQuery query(url);
    for (const QString &key : kSecretKeys) {
        if (query.hasQueryItem(key)) {
            query.removeAllQueryItems(key);
            query.addQueryItem(key, u"…"_s);
        }
    }
    QUrl clean = url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveFragment | QUrl::RemoveQuery);
    clean.setQuery(query);
    return clean.toDisplayString(QUrl::DecodeReserved);
}

RetryAdvice defaultInspection(Service service, const HttpResponse &response)
{
    RetryAdvice advice;
    const int status = response.status;
    if (status == 429 || status == 503) {
        advice.kind = RetryAdvice::Kind::Retry;
        advice.delay = parseRetryAfter(response.header("Retry-After"), QDateTime::currentDateTimeUtc());
        advice.blockService = true;
        advice.error = makeError(service, ErrorCode::RateLimited,
                                 u"%1 asked Selecta to slow down."_s.arg(serviceName(service)), status);
    } else if (status == 500 || status == 502 || status == 504) {
        advice.kind = RetryAdvice::Kind::Retry;
        advice.error =
            makeError(service, ErrorCode::ServerError, u"%1 had a server error."_s.arg(serviceName(service)), status);
    }
    return advice;
}

MetaError statusError(Service service, const HttpResponse &response)
{
    const int status = response.status;
    const QString name = serviceName(service);
    if (status == 400) {
        return makeError(service, ErrorCode::InvalidArgument, u"%1 rejected the request."_s.arg(name), status);
    }
    if (status == 401) {
        return makeError(service, ErrorCode::SignInRequired, u"%1 needs a valid sign-in."_s.arg(name), status);
    }
    if (status == 403) {
        return makeError(service, ErrorCode::Forbidden, u"%1 refused access."_s.arg(name), status);
    }
    if (status == 404) {
        return makeError(service, ErrorCode::NotFound, u"%1 has no such item."_s.arg(name), status);
    }
    if (status == 429) {
        return makeError(service, ErrorCode::RateLimited, u"%1 asked Selecta to slow down."_s.arg(name), status);
    }
    if (status >= 500 && status <= 599) {
        return makeError(service, ErrorCode::ServerError, u"%1 had a server error."_s.arg(name), status);
    }
    return makeError(service, ErrorCode::BadResponse, u"%1 answered HTTP %2."_s.arg(name).arg(status), status);
}

struct HttpChannel::Op
{
    RequestId id = 0;
    HttpRequest request;
    Done done;
    int retries = 0;
    int hops = 0;
    quint64 ticket = 0;
    enum class Stage : quint8 {
        Queued,   // waiting for the gate (and any back-off)
        Permit,   // waiting for the governor
        InFlight, // the reply is running
    } stage = Stage::Queued;
    QPointer<QNetworkReply> reply;
    QByteArray buffer;
    Abort abort = Abort::None;
    base::Clock::TimePoint sentAt;
};

HttpChannel::HttpChannel(Service service, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
                         const base::Clock &clock, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_policy(policyFor(service))
    , m_network(network)
    , m_pacer(pacer)
    , m_cache(cache)
    , m_clock(clock)
    , m_gate(std::make_unique<ServiceGate>(
          ServiceGate::Limits{m_policy.minSpacing, m_policy.windowCount, m_policy.window, m_policy.maxInFlight}, clock))
    , m_timeout(m_policy.requestTimeout)
{
    Q_ASSERT_X(operationFor(service).has_value(), "HttpChannel", "a network service is required");
}

HttpChannel::~HttpChannel()
{
    // Nothing calls back once the channel is gone; replies are aborted quietly.
    const QHash<RequestId, std::shared_ptr<Op>> ops = std::exchange(m_ops, {});
    for (const auto &op : ops) {
        if (QNetworkReply *reply = op->reply.data()) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
}

MetaError HttpChannel::error(ErrorCode code, QString message, int httpStatus) const
{
    return makeError(m_service, code, std::move(message), httpStatus);
}

std::optional<MetaError> HttpChannel::blockedError() const
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_gate->blockRemaining());
    if (remaining <= m_policy.maxWait) {
        return std::nullopt;
    }
    MetaError e = error(ErrorCode::RateLimited, u"%1 asked Selecta to wait %2 before the next request."_s.arg(
                                                    serviceName(m_service), describeWait(remaining)));
    e.retryAfter = roundUpSeconds(remaining);
    return e;
}

std::chrono::milliseconds HttpChannel::backoff(int attempt) const
{
    std::chrono::milliseconds wait = m_policy.firstBackoff;
    for (int i = 0; i < attempt && wait < m_policy.maxBackoff; ++i) {
        wait *= 2;
    }
    return std::min(wait, m_policy.maxBackoff);
}

void HttpChannel::failLater(Done done, MetaError e)
{
    QMetaObject::invokeMethod(this, [done = std::move(done), e = std::move(e)] { done(e); }, Qt::QueuedConnection);
}

void HttpChannel::report(const HttpRequest &request, int status, qint64 bytes, std::chrono::milliseconds elapsed,
                         const QString &outcome)
{
    RequestRecord record;
    record.service = m_service;
    record.method = request.method;
    record.target = request.logTarget.isEmpty() ? scrubbedTarget(request.url) : request.logTarget;
    record.httpStatus = status;
    record.bytes = bytes;
    record.elapsed = elapsed;
    record.outcome = outcome;
    qCDebug(lcMetaHttp).noquote() << serviceName(m_service) << request.method << record.target << outcome;
    m_pacer.report(record);
}

HttpChannel::RequestId HttpChannel::send(HttpRequest request, Done done)
{
    const RequestId id = m_nextId++;
    if (m_network.isNull()) {
        failLater(std::move(done), error(ErrorCode::NotConfigured, u"No network access manager."_s));
        return id;
    }
    if (!request.url.isValid() || request.url.scheme() != u"https"_s) {
        // Every service is HTTPS; tests use https:// URLs answered by a fake network manager.
        failLater(std::move(done), error(ErrorCode::Internal, u"Refusing a non-HTTPS URL."_s));
        return id;
    }
    if (m_cache != nullptr && !request.cacheKey.isEmpty() && request.method == "GET") {
        std::optional<QByteArray> cached = m_cache->get(request.cacheKey);
        int status = 200;
        if (!cached.has_value() && request.notFoundTtl > 0s) {
            cached = m_cache->get(request.cacheKey + kNotFoundSuffix);
            status = 404;
        }
        if (cached.has_value()) {
            HttpResponse response;
            response.status = status;
            response.body = std::move(*cached);
            response.url = request.url;
            response.fromCache = true;
            report(request, status, response.body.size(), 0ms, u"cached"_s);
            QMetaObject::invokeMethod(
                this, [done = std::move(done), response = std::move(response)] { done(response); },
                Qt::QueuedConnection);
            return id;
        }
    }
    if (std::optional<MetaError> blocked = blockedError()) {
        report(request, 0, 0, 0ms, u"not sent: %1"_s.arg(blocked->message));
        failLater(std::move(done), std::move(*blocked));
        return id;
    }
    const auto op = std::make_shared<Op>();
    op->id = id;
    op->request = std::move(request);
    op->done = std::move(done);
    m_ops.insert(id, op);
    enqueueAttempt(op, false);
    return id;
}

void HttpChannel::enqueueAttempt(const std::shared_ptr<Op> &op, bool front)
{
    op->stage = Op::Stage::Queued;
    const RequestId id = op->id;
    op->ticket = m_gate->enqueue([this, id](quint64 ticket) { onGateReady(id, ticket); }, front);
}

void HttpChannel::onGateReady(RequestId id, quint64 ticket)
{
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->ticket != ticket || op->stage != Op::Stage::Queued) {
        m_gate->abandon(ticket);
        return;
    }
    op->stage = Op::Stage::Permit;
    const QPointer<HttpChannel> guard(this);
    const auto operation = operationFor(m_service);
    m_pacer.acquire(operation.value_or(governor::Operation::YtDlpRun), [guard, id, ticket](PermitDecision decision) {
        if (!guard.isNull()) {
            guard->onPermit(id, ticket, decision);
        }
    });
}

void HttpChannel::onPermit(RequestId id, quint64 ticket, PermitDecision &decision)
{
    std::optional<governor::Permit> permit = std::move(decision.permit);
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->ticket != ticket || op->stage != Op::Stage::Permit) {
        return; // cancelled meanwhile (the gate was released then); the unused permit is dropped
    }
    if (!permit.has_value()) {
        m_gate->abandon(ticket);
        if (decision.retryAfter > 0ms && decision.retryAfter <= m_policy.maxWait) {
            // The governor's budget refills soon: hold every request to this service until then, keep our place.
            m_gate->blockUntil(m_clock.now() + decision.retryAfter);
            enqueueAttempt(op, true);
            return;
        }
        MetaError e = error(ErrorCode::Refused,
                            decision.refusal.isEmpty() ? u"The network governor refused."_s : decision.refusal);
        if (decision.retryAfter > 0ms) {
            e.retryAfter = roundUpSeconds(decision.retryAfter);
        }
        report(op->request, 0, 0, 0ms, u"refused: %1"_s.arg(e.message));
        complete(op, e);
        return;
    }
    if (!permit->consume(operationFor(m_service).value_or(governor::Operation::YtDlpRun))) {
        m_gate->abandon(ticket);
        complete(op, error(ErrorCode::Internal, u"The governor issued a permit for another operation."_s));
        return;
    }
    m_gate->markStarted(ticket);
    startReply(op);
}

void HttpChannel::startReply(const std::shared_ptr<Op> &op)
{
    if (m_network.isNull()) {
        m_gate->markFinished(op->ticket);
        complete(op, error(ErrorCode::NotConfigured, u"The network access manager is gone."_s));
        return;
    }
    QNetworkRequest request(op->request.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    for (const auto &[name, value] : std::as_const(op->request.headers)) {
        request.setRawHeader(name, value);
    }
    op->buffer.clear();
    op->abort = Abort::None;
    op->stage = Op::Stage::InFlight;
    op->sentAt = m_clock.now();
    QNetworkReply *reply = m_network->sendCustomRequest(request, op->request.method, op->request.body);
    op->reply = reply;

    const RequestId id = op->id;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, id, reply] {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > m_policy.maxBodyBytes) {
            abortReply(id, Abort::TooLarge);
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, id] { onReadyRead(id); });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply] { onFinished(id, reply); });
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id] { abortReply(id, Abort::Timeout); });
    timer->start(m_timeout);
}

void HttpChannel::onReadyRead(RequestId id)
{
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->reply.isNull() || op->abort != Abort::None) {
        return;
    }
    const qint64 room = m_policy.maxBodyBytes - op->buffer.size();
    op->buffer += op->reply->read(room + 1);
    if (op->buffer.size() > m_policy.maxBodyBytes) {
        abortReply(id, Abort::TooLarge);
    }
}

void HttpChannel::abortReply(RequestId id, Abort reason)
{
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->reply.isNull() || op->abort != Abort::None) {
        return;
    }
    op->abort = reason;
    op->reply->abort(); // finished() follows, now or on a later turn
}

void HttpChannel::onFinished(RequestId id, QNetworkReply *reply)
{
    reply->deleteLater();
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->reply != reply) {
        return;
    }
    op->reply = nullptr;
    m_gate->markFinished(op->ticket);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(m_clock.now() - op->sentAt);

    if (op->abort == Abort::Timeout) {
        report(op->request, 0, op->buffer.size(), elapsed, u"timeout"_s);
        complete(op, error(ErrorCode::Timeout, u"%1 didn't answer in time."_s.arg(serviceName(m_service))));
        return;
    }
    if (op->abort == Abort::TooLarge) {
        report(op->request, 0, op->buffer.size(), elapsed, u"aborted: answer too large"_s);
        complete(op,
                 error(ErrorCode::TooLarge, u"The answer is larger than %1 MiB."_s.arg(m_policy.maxBodyBytes / kMiB)));
        return;
    }

    const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusAttribute.isValid()) {
        const QString reason = reply->errorString();
        report(op->request, 0, 0, elapsed, u"network error: %1"_s.arg(reason));
        complete(op, error(networkErrorCode(reply->error()),
                           u"Couldn't reach %1 (%2)."_s.arg(serviceName(m_service), reason)));
        return;
    }
    if (op->buffer.size() <= m_policy.maxBodyBytes) {
        op->buffer += reply->read(m_policy.maxBodyBytes - op->buffer.size() + 1);
    }
    if (op->buffer.size() > m_policy.maxBodyBytes) {
        report(op->request, statusAttribute.toInt(), op->buffer.size(), elapsed, u"aborted: answer too large"_s);
        complete(op,
                 error(ErrorCode::TooLarge, u"The answer is larger than %1 MiB."_s.arg(m_policy.maxBodyBytes / kMiB)));
        return;
    }

    HttpResponse response;
    response.status = statusAttribute.toInt();
    response.body = std::move(op->buffer);
    op->buffer = {};
    for (const auto &pair : reply->rawHeaderPairs()) {
        response.headers.append({pair.first, pair.second});
    }
    response.url = op->request.url;

    if (isRedirect(response.status)) {
        followRedirect(op, response, elapsed);
        return;
    }
    handleAnswer(op, std::move(response), elapsed);
}

void HttpChannel::followRedirect(const std::shared_ptr<Op> &op, const HttpResponse &response,
                                 std::chrono::milliseconds elapsed)
{
    const QByteArray location = response.header("Location");
    const QUrl target = op->request.url.resolved(QUrl::fromEncoded(location));
    report(op->request, response.status, response.body.size(), elapsed, u"redirect"_s);
    if (location.isEmpty() || !target.isValid()) {
        complete(op, error(ErrorCode::BadResponse, u"A redirect without a usable target."_s, response.status));
        return;
    }
    if (target.scheme() != u"https"_s) {
        complete(op,
                 error(ErrorCode::BadResponse, u"A redirect to a non-HTTPS address was refused."_s, response.status));
        return;
    }
    if (++op->hops > m_policy.maxRedirects) {
        complete(op, error(ErrorCode::BadResponse, u"Too many redirects."_s, response.status));
        return;
    }
    if (target.host().compare(op->request.url.host(), Qt::CaseInsensitive) != 0) {
        op->request.headers.removeIf(
            [](const auto &header) { return header.first.compare("Authorization", Qt::CaseInsensitive) == 0; });
    }
    if (response.status == 303) {
        op->request.method = QByteArrayLiteral("GET");
        op->request.body.clear();
    }
    op->request.url = target;
    enqueueAttempt(op, true); // a new hop: paced and permitted like any request
}

void HttpChannel::handleAnswer(const std::shared_ptr<Op> &op, HttpResponse response, std::chrono::milliseconds elapsed)
{
    const RetryAdvice advice = m_inspector ? m_inspector(response) : defaultInspection(m_service, response);
    if (advice.kind == RetryAdvice::Kind::Accept) {
        report(op->request, response.status, response.body.size(), elapsed,
               response.status >= 200 && response.status <= 299 ? u"ok"_s : u"HTTP %1"_s.arg(response.status));
        if (m_cache != nullptr && !op->request.cacheKey.isEmpty() && op->request.method == "GET") {
            if (response.status == 200 && op->request.cacheTtl > 0s) {
                m_cache->put(op->request.cacheKey, response.body, op->request.cacheTtl);
            } else if (response.status == 404 && op->request.notFoundTtl > 0s) {
                m_cache->put(op->request.cacheKey + kNotFoundSuffix, {}, op->request.notFoundTtl);
            }
        }
        complete(op, std::move(response));
        return;
    }

    const std::chrono::milliseconds wait = advice.delay.value_or(backoff(op->retries));
    // A retry backs off the whole service: a struggling or rate-limiting server gets a break from every request.
    if ((advice.blockService || advice.kind == RetryAdvice::Kind::Retry) && wait > 0ms) {
        m_gate->blockUntil(m_clock.now() + wait);
    }
    MetaError failure = advice.error.value_or(statusError(m_service, response));
    if (failure.httpStatus == 0) {
        failure.httpStatus = response.status;
    }
    if (advice.kind == RetryAdvice::Kind::Fail) {
        if (advice.delay.has_value()) {
            failure.retryAfter = roundUpSeconds(*advice.delay);
        }
        report(op->request, response.status, response.body.size(), elapsed, failure.message);
        complete(op, failure);
        return;
    }
    if (op->retries >= m_policy.maxRetries || wait > m_policy.maxWait) {
        if (failure.code == ErrorCode::RateLimited || advice.delay.has_value()) {
            failure.retryAfter = roundUpSeconds(wait);
        }
        report(op->request, response.status, response.body.size(), elapsed,
               u"HTTP %1, giving up (wait %2)"_s.arg(response.status).arg(describeWait(wait)));
        complete(op, failure);
        return;
    }
    ++op->retries;
    report(op->request, response.status, response.body.size(), elapsed,
           u"HTTP %1, retrying in %2"_s.arg(response.status).arg(describeWait(wait)));
    enqueueAttempt(op, true); // the gate holds it until the back-off ends
}

void HttpChannel::complete(const std::shared_ptr<Op> &op, Result<HttpResponse> result)
{
    m_ops.remove(op->id);
    if (op->done) {
        const Done done = std::move(op->done);
        op->done = nullptr;
        done(std::move(result));
    }
}

void HttpChannel::cancel(RequestId id)
{
    const std::shared_ptr<Op> op = m_ops.take(id);
    if (!op) {
        return;
    }
    op->done = nullptr;
    switch (op->stage) {
    case Op::Stage::Queued:
    case Op::Stage::Permit:
        m_gate->abandon(op->ticket);
        break;
    case Op::Stage::InFlight:
        m_gate->markFinished(op->ticket);
        if (QNetworkReply *reply = op->reply.data()) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
        break;
    }
}

void HttpChannel::blockFor(std::chrono::milliseconds delay)
{
    if (delay > 0ms) {
        m_gate->blockUntil(m_clock.now() + delay);
    }
}

base::Clock::Duration HttpChannel::blockRemaining() const
{
    return m_gate->blockRemaining();
}

} // namespace selecta::meta::detail
