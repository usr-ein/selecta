// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/DeezerClient.h"

#include "meta/MetaCache.h"
#include "meta/ServicePolicy.h"
#include "meta/detail/Async.h"
#include "meta/detail/DeezerJson.h"
#include "meta/detail/HttpChannel.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/Json.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::meta {

namespace dzjson = detail::deezer;

namespace {

constexpr qsizetype kMaxQueryLength = 200;

MetaError dzError(ErrorCode code, QString message, int status = 0)
{
    return makeError(Service::Deezer, code, std::move(message), status);
}

// Deezer answers errors with HTTP 200 and {"error": {"code": …}}: a quota or busy error backs the service off and
// retries; "no data" fails with NotFound and anything else with BadResponse. Failing here also keeps error answers,
// and bodies that aren't JSON, out of the cache.
detail::RetryAdvice inspect(const detail::HttpResponse &response)
{
    if (response.status == 200 && response.body.size() < 4096 && response.body.contains("\"error\"")) {
        const Result<QJsonObject> json = detail::json::parseObject(response.body, Service::Deezer);
        const std::optional<int> code = json.ok() ? dzjson::errorCode(json.value()) : std::nullopt;
        if (code.has_value()) {
            detail::RetryAdvice advice;
            if (*code == dzjson::kQuotaError || *code == dzjson::kServiceBusyError) {
                advice.kind = detail::RetryAdvice::Kind::Retry;
                advice.error = dzError(ErrorCode::RateLimited, u"Deezer asked Selecta to slow down."_s);
                return advice;
            }
            advice.kind = detail::RetryAdvice::Kind::Fail;
            advice.error = *code == dzjson::kNotFoundError
                               ? dzError(ErrorCode::NotFound, u"Deezer has no such item."_s)
                               : dzError(ErrorCode::BadResponse, u"Deezer refused the request (code %1)."_s.arg(*code));
            return advice;
        }
    }
    return detail::jsonInspection(Service::Deezer, response);
}

} // namespace

struct DeezerClient::Private
{
    using JsonDone = std::function<void(Result<QJsonObject>)>;

    DeezerClient *q;
    QUrl api;
    std::unique_ptr<detail::HttpChannel> channel;

    Private(DeezerClient *owner, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
            const base::Clock &clock, QUrl apiUrl)
        : q(owner)
        , api(std::move(apiUrl))
        , channel(std::make_unique<detail::HttpChannel>(Service::Deezer, network, pacer, cache, clock))
    {
        channel->setInspector(inspect);
    }

    // A GET; `ttl` 0 bypasses the cache both ways. `done` runs exactly once, never inside this call.
    void get(const QString &path, const QUrlQuery &query, std::chrono::seconds ttl, Priority priority,
             const detail::CancelSlot &slot, const JsonDone &done)
    {
        QUrl url = api;
        url.setPath(api.path() + path);
        url.setQuery(query);
        detail::HttpRequest http;
        http.url = std::move(url);
        http.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")});
        if (ttl > 0s) {
            http.cacheKey = u"deezer"_s + path + u'?' + query.query(QUrl::FullyEncoded);
            http.cacheTtl = ttl;
        }
        http.priority = priority;
        const auto callback = std::make_shared<JsonDone>(done);
        const auto id = channel->send(std::move(http), [callback, slot](const Result<detail::HttpResponse> &result) {
            slot.clear();
            if (!result.ok()) {
                (*callback)(result.error());
                return;
            }
            if (result.value().status != 200) {
                (*callback)(detail::statusError(Service::Deezer, result.value()));
                return;
            }
            (*callback)(detail::json::parseObject(result.value().body, Service::Deezer));
        });
        slot.set([this, id] { channel->cancel(id); });
    }

    template <typename T>
    QFuture<Result<T>> run(const QString &path, const QUrlQuery &query, std::chrono::seconds ttl, Priority priority,
                           const std::function<Result<T>(const QJsonObject &)> &map)
    {
        const detail::SharedPromise<Result<T>> promise;
        const detail::CancelSlot slot;
        promise.onCanceled(q, [slot] { slot.cancel(); });
        get(path, query, ttl, priority, slot, [promise, map](const Result<QJsonObject> &result) {
            if (!result.ok()) {
                promise.finish(result.error());
                return;
            }
            promise.finish(map(result.value()));
        });
        return promise.future();
    }
};

DeezerClient::DeezerClient(QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
                           const base::Clock &clock, QObject *parent, QUrl apiUrl)
    : QObject(parent)
    , d(std::make_unique<Private>(this, network, pacer, cache, clock, std::move(apiUrl)))
{}

DeezerClient::~DeezerClient() = default;

QString DeezerClient::cleanQuery(const QString &query)
{
    QString text = detail::json::cleanText(query, kMaxQueryLength);
    text.replace(u'"', u' ');
    text.replace(u':', u' ');
    return text.simplified();
}

namespace {

Result<DeezerTrack> oneTrack(const QJsonObject &object)
{
    std::optional<DeezerTrack> track = dzjson::track(object);
    if (!track.has_value()) {
        return dzError(ErrorCode::BadResponse, u"Deezer's answer has no track in it."_s);
    }
    return std::move(*track);
}

} // namespace

QFuture<Result<QList<DeezerTrack>>> DeezerClient::search(const QString &query, int limit, Priority priority)
{
    const QString text = cleanQuery(query);
    if (text.isEmpty()) {
        return detail::deferred<Result<QList<DeezerTrack>>>(
            this, dzError(ErrorCode::InvalidArgument, u"A Deezer search needs some text."_s));
    }
    QUrlQuery parameters;
    parameters.addQueryItem(u"q"_s, text);
    parameters.addQueryItem(u"limit"_s, QString::number(std::clamp(limit, 1, kMaxSearchLimit)));
    return d->run<QList<DeezerTrack>>(u"/search"_s, parameters, kSearchTtl, priority,
                                      [](const QJsonObject &object) { return dzjson::searchHits(object); });
}

QFuture<Result<DeezerTrack>> DeezerClient::trackByIsrc(const QString &isrc, bool fresh, Priority priority)
{
    const QString normalized = detail::normalizedIsrc(isrc);
    if (normalized.isEmpty()) {
        return detail::deferred<Result<DeezerTrack>>(
            this, dzError(ErrorCode::InvalidArgument, u"\"%1\" isn't an ISRC."_s.arg(isrc.left(40))));
    }
    return d->run<DeezerTrack>(u"/track/isrc:"_s + normalized, {}, fresh ? 0s : kEntityTtl, priority, oneTrack);
}

QFuture<Result<DeezerTrack>> DeezerClient::track(const QString &trackId, bool fresh, Priority priority)
{
    static const QRegularExpression kDigits(u"^[1-9][0-9]{0,18}$"_s);
    if (!kDigits.match(trackId).hasMatch()) {
        return detail::deferred<Result<DeezerTrack>>(
            this, dzError(ErrorCode::InvalidArgument, u"That isn't a Deezer track ID."_s));
    }
    return d->run<DeezerTrack>(u"/track/"_s + trackId, {}, fresh ? 0s : kEntityTtl, priority, oneTrack);
}

QFuture<Result<DeezerAlbum>> DeezerClient::album(const QString &albumId, Priority priority)
{
    static const QRegularExpression kDigits(u"^[1-9][0-9]{0,18}$"_s);
    if (!kDigits.match(albumId).hasMatch()) {
        return detail::deferred<Result<DeezerAlbum>>(
            this, dzError(ErrorCode::InvalidArgument, u"That isn't a Deezer album ID."_s));
    }
    return d->run<DeezerAlbum>(u"/album/"_s + albumId, {}, kEntityTtl, priority,
                               [](const QJsonObject &object) -> Result<DeezerAlbum> {
                                   std::optional<DeezerAlbum> album = dzjson::album(object);
                                   if (!album.has_value()) {
                                       return dzError(ErrorCode::BadResponse, u"Deezer's answer has no album."_s);
                                   }
                                   return std::move(*album);
                               });
}

QDateTime DeezerClient::previewExpiry(const QUrl &previewUrl, const QDateTime &fallback)
{
    // Signed CDN links carry "hdnea=exp=<unix seconds>~acl=…~hmac=…".
    static const QRegularExpression kExpiry(u"(?:^|[~?&=])exp=([0-9]{9,11})(?:[~&]|$)"_s);
    const QRegularExpressionMatch match = kExpiry.match(previewUrl.query(QUrl::FullyDecoded));
    if (!match.hasMatch()) {
        return fallback;
    }
    bool ok = false;
    const qint64 seconds = match.captured(1).toLongLong(&ok);
    if (!ok || seconds <= 0) {
        return fallback;
    }
    return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
}

} // namespace selecta::meta
