// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/SpotifyClient.h"

#include "meta/MetaCache.h"
#include "meta/ServicePolicy.h"
#include "meta/SpotifyAuth.h"
#include "meta/detail/Async.h"
#include "meta/detail/HttpChannel.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/Json.h"
#include "meta/detail/SpotifyJson.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::meta {

namespace {

constexpr int kMaxPlaylists = 1000;
constexpr qsizetype kMaxFieldLength = 100;
constexpr std::chrono::hours kQuotaBlockDefault{1};

MetaError apiError(ErrorCode code, QString message, int status = 0)
{
    return makeError(Service::SpotifyApi, code, std::move(message), status);
}

// A search filter value: no quotes or control characters, single spaces, bounded.
QString filterValue(const QString &text)
{
    QString value = detail::json::cleanText(text, kMaxFieldLength);
    value.remove(u'"');
    return value.simplified();
}

// Spotify reports its per-account quota as 429 with reason QUOTA_EXCEEDED: stop all Spotify calls until Retry-After
// (an hour when absent) instead of retrying into it.
detail::RetryAdvice inspect(const detail::HttpResponse &response)
{
    if (response.status == 429) {
        const Result<QJsonObject> json = detail::json::parseObject(response.body, Service::SpotifyApi);
        QString reason;
        if (json.ok()) {
            reason = detail::json::text(detail::json::object(json.value(), "error"_L1), "reason"_L1, 64);
            if (reason.isEmpty()) {
                reason = detail::json::text(json.value(), "reason"_L1, 64);
            }
        }
        if (reason == u"QUOTA_EXCEEDED"_s) {
            detail::RetryAdvice advice;
            advice.kind = detail::RetryAdvice::Kind::Fail;
            advice.blockService = true;
            advice.delay = detail::parseRetryAfter(response.header("Retry-After"), QDateTime::currentDateTimeUtc())
                               .value_or(std::chrono::duration_cast<std::chrono::milliseconds>(kQuotaBlockDefault));
            advice.error = apiError(ErrorCode::QuotaExceeded,
                                    u"Your Spotify developer account used up its quota; Spotify pauses Selecta's "
                                    u"requests for a while."_s,
                                    429);
            return advice;
        }
    }
    return detail::defaultInspection(Service::SpotifyApi, response);
}

} // namespace

struct SpotifyClient::Private
{
    struct Get
    {
        QString path; // relative to /v1
        QUrlQuery query;
        std::chrono::seconds ttl{0}; // 0: never cached (user data)
        QStringList scopes;          // needed scopes
        QString forbidden;           // the message for a 403
        Priority priority = Priority::Interactive;
    };
    using JsonDone = std::function<void(Result<QJsonObject>)>;

    SpotifyClient *q;
    SpotifyAuth &auth;
    MetaCache *cache;
    QUrl api;
    std::unique_ptr<detail::HttpChannel> channel;

    Private(SpotifyClient *owner, SpotifyAuth &a, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *c,
            const base::Clock &clock, QUrl apiUrl)
        : q(owner)
        , auth(a)
        , cache(c)
        , api(std::move(apiUrl))
        , channel(std::make_unique<detail::HttpChannel>(Service::SpotifyApi, network, pacer, c, clock))
    {
        channel->setInspector(inspect);
    }

    void later(const JsonDone &done, MetaError error) const
    {
        QMetaObject::invokeMethod(q, [done, error = std::move(error)] { done(error); }, Qt::QueuedConnection);
    }

    static Result<QJsonObject> parse(const QByteArray &body)
    {
        return detail::json::parseObject(body, Service::SpotifyApi);
    }

    // An authenticated GET: access token, request, one token refresh and retry on 401. `done` runs exactly once,
    // never inside this call, unless `slot` is cancelled first.
    void get(const Get &request, const detail::CancelSlot &slot, const JsonDone &done, bool retried = false)
    {
        Q_ASSERT_X(detail::spotify::isDevelopmentModeEndpoint(request.path), "SpotifyClient",
                   "an endpoint outside development mode's list");
        if (slot.isCancelled()) {
            return;
        }
        if (auth.mode() == SpotifyAuth::Mode::None) {
            later(done, apiError(ErrorCode::SignInRequired, u"Sign in to Spotify first (Settings → Integrations)."_s));
            return;
        }
        if (!auth.hasScopes(request.scopes)) {
            later(done, apiError(ErrorCode::MissingScope, auth.mode() == SpotifyAuth::Mode::ClientCredentials
                                                              ? u"This needs a Spotify sign-in with your account."_s
                                                              : u"Sign in to Spotify again to allow this."_s));
            return;
        }
        QUrl url = api;
        url.setPath(api.path() + request.path);
        url.setQuery(request.query);
        const QString cacheKey = u"spotify"_s + request.path + u'?' + request.query.query(QUrl::FullyEncoded);
        if (request.ttl > 0s && cache != nullptr) {
            if (const std::optional<QByteArray> cached = cache->get(cacheKey)) {
                QMetaObject::invokeMethod(q, [done, body = *cached] { done(parse(body)); }, Qt::QueuedConnection);
                return;
            }
        }
        const auto callback = std::make_shared<JsonDone>(done);
        detail::whenReady(auth.accessToken(), q,
                          [this, request, slot, callback, retried, url, cacheKey](const Result<QString> &token) {
                              if (slot.isCancelled()) {
                                  return;
                              }
                              if (!token.ok()) {
                                  (*callback)(token.error());
                                  return;
                              }
                              send(request, slot, callback, retried, url, cacheKey, token.value());
                          });
    }

    void send(const Get &request, const detail::CancelSlot &slot, const std::shared_ptr<JsonDone> &callback,
              bool retried, const QUrl &url, const QString &cacheKey, const QString &accessToken)
    {
        detail::HttpRequest http;
        http.url = url;
        http.priority = request.priority;
        http.headers.append({QByteArrayLiteral("Authorization"), "Bearer " + accessToken.toUtf8()});
        http.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")});
        if (request.ttl > 0s) {
            http.cacheKey = cacheKey;
            http.cacheTtl = request.ttl;
        }
        const auto id = channel->send(std::move(http), [this, request, slot, callback, retried,
                                                        accessToken](const Result<detail::HttpResponse> &result) {
            slot.clear();
            answer(request, slot, callback, retried, accessToken, result);
        });
        slot.set([this, id] { channel->cancel(id); });
    }

    void answer(const Get &request, const detail::CancelSlot &slot, const std::shared_ptr<JsonDone> &callback,
                bool retried, const QString &accessToken, const Result<detail::HttpResponse> &result)
    {
        const JsonDone &done = *callback;
        if (!result.ok()) {
            done(result.error());
            return;
        }
        const detail::HttpResponse &response = result.value();
        if (response.status == 401) {
            auth.reportUnauthorized(accessToken);
            if (!retried) {
                get(request, slot, done, true);
                return;
            }
            done(apiError(ErrorCode::SignInRequired, u"Spotify rejected the sign-in; sign in again."_s, 401));
            return;
        }
        if (response.status == 403) {
            done(apiError(ErrorCode::Forbidden,
                          request.forbidden.isEmpty()
                              ? u"Spotify refused access. If this account isn't the app's owner, add it under User "
                                u"Management in the Spotify developer dashboard."_s
                              : request.forbidden,
                          403));
            return;
        }
        if (response.status != 200) {
            done(detail::statusError(Service::SpotifyApi, response));
            return;
        }
        done(parse(response.body));
    }

    // A paged list (offset/limit, items/total/next): `consume` sees every item until it returns false (a bound) or
    // the list ends.
    struct Pager
    {
        Get base;
        int pageSize = kPageSize;
        std::function<bool(const QJsonObject &item)> consume;
        std::function<void(Result<void>)> done;
    };

    void page(const std::shared_ptr<Pager> &pager, int offset, const detail::CancelSlot &slot)
    {
        Get request = pager->base;
        request.query.addQueryItem(u"limit"_s, QString::number(pager->pageSize));
        request.query.addQueryItem(u"offset"_s, QString::number(offset));
        get(request, slot, [this, pager, offset, slot](const Result<QJsonObject> &result) {
            if (!result.ok()) {
                pager->done(result.error());
                return;
            }
            const QJsonArray items = detail::json::array(result.value(), "items"_L1);
            for (const auto &value : items) {
                if (!pager->consume(value.toObject())) {
                    pager->done({}); // bounded: stop here
                    return;
                }
            }
            const qint64 total = detail::json::integer(result.value(), "total"_L1).value_or(0);
            const int next = offset + static_cast<int>(items.size());
            if (items.isEmpty() || result.value().value("next"_L1).isNull() || next >= total) {
                pager->done({});
                return;
            }
            page(pager, next, slot);
        });
    }

    void albumWithTracks(const QString &albumId, Priority priority, const detail::CancelSlot &slot,
                         const std::function<void(Result<Album>)> &whenDone)
    {
        const auto callback = std::make_shared<std::function<void(Result<Album>)>>(whenDone);
        get({u"/albums/"_s + albumId, {}, kEntityTtl, {}, {}, priority}, slot,
            [this, albumId, priority, slot, callback](const Result<QJsonObject> &result) {
                const auto &done = *callback;
                if (!result.ok()) {
                    done(result.error());
                    return;
                }
                const auto album = std::make_shared<Album>(detail::spotify::album(result.value()));
                if (album->name.isEmpty()) {
                    done(apiError(ErrorCode::BadResponse, u"Spotify's album answer has no album in it."_s));
                    return;
                }
                const QJsonObject firstPage = detail::json::object(result.value(), "tracks"_L1);
                const qint64 total = detail::json::integer(firstPage, "total"_L1).value_or(album->tracks.size());
                if (album->truncated || album->tracks.size() >= total) {
                    detail::spotify::finishAlbum(*album);
                    done(*album);
                    return;
                }
                const auto pager = std::make_shared<Pager>();
                pager->base = {u"/albums/"_s + albumId + u"/tracks"_s, {}, kEntityTtl, {}, {}, priority};
                pager->consume = [album](const QJsonObject &item) {
                    detail::spotify::appendAlbumTracks(*album, QJsonArray{item});
                    return !album->truncated;
                };
                pager->done = [album, callback](const Result<void> &paged) {
                    if (!paged.ok()) {
                        (*callback)(paged.error());
                        return;
                    }
                    detail::spotify::finishAlbum(*album);
                    (*callback)(*album);
                };
                page(pager, static_cast<int>(album->tracks.size()), slot);
            });
    }

    void playlistTracks(const QString &playlistId, Priority priority, const detail::CancelSlot &slot,
                        const std::function<void(Result<TrackList>)> &whenDone)
    {
        const auto callback = std::make_shared<std::function<void(Result<TrackList>)>>(whenDone);
        const QString forbidden = u"Spotify only lets Selecta read playlists you own or collaborate on. Select the "
                                  u"playlist's songs in Spotify, copy them (⌘C) and paste the links instead."_s;
        QUrlQuery fields;
        fields.addQueryItem(u"fields"_s, u"name"_s);
        get({u"/playlists/"_s + playlistId, fields, 0s, SpotifyAuth::playlistScopes(), forbidden, priority}, slot,
            [this, playlistId, priority, slot, callback, forbidden](const Result<QJsonObject> &meta) {
                if (!meta.ok()) {
                    (*callback)(meta.error());
                    return;
                }
                const auto list = std::make_shared<TrackList>();
                list->name = detail::json::text(meta.value(), "name"_L1);
                const auto pager = std::make_shared<Pager>();
                QUrlQuery query;
                query.addQueryItem(u"additional_types"_s, u"track"_s);
                pager->base = {u"/playlists/"_s + playlistId + u"/items"_s,
                               query,
                               0s,
                               SpotifyAuth::playlistScopes(),
                               forbidden,
                               priority};
                pager->consume = [list](const QJsonObject &item) { return collect(*list, item); };
                pager->done = [list, callback](const Result<void> &paged) {
                    if (!paged.ok()) {
                        (*callback)(paged.error());
                        return;
                    }
                    (*callback)(*list);
                };
                page(pager, 0, slot);
            });
    }

    // Adds a playlist or library item to `list`; false once the bound is reached.
    static bool collect(TrackList &list, const QJsonObject &item)
    {
        if (list.tracks.size() >= kMaxImportTracks) {
            list.truncated = true;
            return false;
        }
        if (std::optional<Release> release = detail::spotify::itemTrack(item)) {
            list.tracks.append(std::move(*release));
        } else {
            ++list.skipped;
        }
        return true;
    }
};

SpotifyClient::SpotifyClient(SpotifyAuth &auth, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
                             const base::Clock &clock, QObject *parent, QUrl apiUrl)
    : QObject(parent)
    , d(std::make_unique<Private>(this, auth, network, pacer, cache, clock, std::move(apiUrl)))
{
    connect(&auth, &SpotifyAuth::signedOut, this, [this] {
        if (d->cache != nullptr) {
            d->cache->removePrefix(u"spotify/"_s); // Spotify Developer Policy §I.2
        }
    });
}

SpotifyClient::~SpotifyClient() = default;

bool SpotifyClient::isAvailable() const
{
    return d->auth.mode() != SpotifyAuth::Mode::None;
}

QString SpotifyClient::searchQueryText(const TrackQuery &query)
{
    QStringList parts;
    if (const QString title = filterValue(query.title); !title.isEmpty()) {
        parts.append(u"track:\"%1\""_s.arg(title));
    }
    if (const QString artist = filterValue(query.artist); !artist.isEmpty()) {
        parts.append(u"artist:\"%1\""_s.arg(artist));
    }
    if (const QString album = filterValue(query.album); !album.isEmpty()) {
        parts.append(u"album:\"%1\""_s.arg(album));
    }
    if (const QString version = filterValue(query.version); !version.isEmpty()) {
        parts.append(version);
    }
    return parts.join(u' ');
}

namespace {

QList<Release> searchResults(const QJsonObject &object)
{
    QList<Release> releases;
    const QJsonArray items = detail::json::array(detail::json::object(object, "tracks"_L1), "items"_L1);
    for (const auto &value : items) {
        if (releases.size() >= SpotifyClient::kMaxSearchLimit) {
            break;
        }
        Release release = detail::spotify::track(value.toObject());
        if (!release.spotifyId.isEmpty() && !release.title.isEmpty()) {
            releases.append(std::move(release));
        }
    }
    return releases;
}

} // namespace

QFuture<Result<QList<Release>>> SpotifyClient::searchTracks(const TrackQuery &query)
{
    const detail::SharedPromise<Result<QList<Release>>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const QString text = searchQueryText(query);
    if (filterValue(query.title).isEmpty() && filterValue(query.artist).isEmpty()) {
        promise.finishLater(this, apiError(ErrorCode::InvalidArgument, u"A search needs an artist or a title."_s));
        return promise.future();
    }
    QUrlQuery parameters;
    parameters.addQueryItem(u"q"_s, text);
    parameters.addQueryItem(u"type"_s, u"track"_s);
    parameters.addQueryItem(u"limit"_s, QString::number(std::clamp(query.limit, 1, kMaxSearchLimit)));
    d->get({u"/search"_s, parameters, kSearchTtl, {}, {}, query.priority}, slot,
           [promise](const Result<QJsonObject> &result) {
               if (!result.ok()) {
                   promise.finish(result.error());
                   return;
               }
               promise.finish(searchResults(result.value()));
           });
    return promise.future();
}

QFuture<Result<QList<Release>>> SpotifyClient::searchIsrc(const QString &isrc, Priority priority)
{
    const detail::SharedPromise<Result<QList<Release>>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const QString normalized = detail::normalizedIsrc(isrc);
    if (normalized.isEmpty()) {
        promise.finishLater(this, apiError(ErrorCode::InvalidArgument, u"\"%1\" isn't an ISRC."_s.arg(isrc.left(40))));
        return promise.future();
    }
    QUrlQuery parameters;
    parameters.addQueryItem(u"q"_s, u"isrc:"_s + normalized);
    parameters.addQueryItem(u"type"_s, u"track"_s);
    parameters.addQueryItem(u"limit"_s, QString::number(kMaxSearchLimit));
    d->get({u"/search"_s, parameters, kSearchTtl, {}, {}, priority}, slot,
           [promise](const Result<QJsonObject> &result) {
               if (!result.ok()) {
                   promise.finish(result.error());
                   return;
               }
               promise.finish(searchResults(result.value()));
           });
    return promise.future();
}

QFuture<Result<Release>> SpotifyClient::track(const QString &trackId, Priority priority)
{
    const detail::SharedPromise<Result<Release>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    if (!spotify::isValidId(trackId)) {
        promise.finishLater(this, apiError(ErrorCode::InvalidArgument, u"That isn't a Spotify track ID."_s));
        return promise.future();
    }
    d->get({u"/tracks/"_s + trackId, {}, kEntityTtl, {}, {}, priority}, slot,
           [promise](const Result<QJsonObject> &result) {
               if (!result.ok()) {
                   promise.finish(result.error());
                   return;
               }
               Release release = detail::spotify::track(result.value());
               if (release.title.isEmpty() || release.spotifyId.isEmpty()) {
                   promise.finish(apiError(ErrorCode::BadResponse, u"Spotify's answer has no track in it."_s));
                   return;
               }
               promise.finish(std::move(release));
           });
    return promise.future();
}

QFuture<Result<Album>> SpotifyClient::album(const QString &albumId, Priority priority)
{
    const detail::SharedPromise<Result<Album>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    if (!spotify::isValidId(albumId)) {
        promise.finishLater(this, apiError(ErrorCode::InvalidArgument, u"That isn't a Spotify album ID."_s));
        return promise.future();
    }
    d->albumWithTracks(albumId, priority, slot, [promise](Result<Album> result) { promise.finish(std::move(result)); });
    return promise.future();
}

QFuture<Result<QList<PlaylistSummary>>> SpotifyClient::myPlaylists()
{
    const detail::SharedPromise<Result<QList<PlaylistSummary>>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const auto playlists = std::make_shared<QList<PlaylistSummary>>();
    const auto pager = std::make_shared<Private::Pager>();
    pager->base = {u"/me/playlists"_s, {}, 0s, SpotifyAuth::playlistScopes(), {}};
    pager->consume = [playlists](const QJsonObject &item) {
        if (playlists->size() >= kMaxPlaylists) {
            return false;
        }
        PlaylistSummary summary = detail::spotify::playlist(item);
        if (!summary.id.isEmpty()) {
            playlists->append(std::move(summary));
        }
        return true;
    };
    pager->done = [promise, playlists](const Result<void> &result) {
        if (!result.ok()) {
            promise.finish(result.error());
            return;
        }
        promise.finish(*playlists);
    };
    d->page(pager, 0, slot);
    return promise.future();
}

QFuture<Result<TrackList>> SpotifyClient::playlistTracks(const QString &playlistId)
{
    const detail::SharedPromise<Result<TrackList>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    if (!spotify::isValidId(playlistId)) {
        promise.finishLater(this, apiError(ErrorCode::InvalidArgument, u"That isn't a Spotify playlist ID."_s));
        return promise.future();
    }
    d->playlistTracks(playlistId, Priority::Interactive, slot,
                      [promise](Result<TrackList> result) { promise.finish(std::move(result)); });
    return promise.future();
}

QFuture<Result<TrackList>> SpotifyClient::likedSongs()
{
    const detail::SharedPromise<Result<TrackList>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const auto list = std::make_shared<TrackList>();
    list->name = u"Liked Songs"_s;
    const auto pager = std::make_shared<Private::Pager>();
    pager->base = {u"/me/tracks"_s, {}, 0s, SpotifyAuth::libraryScopes(), {}};
    pager->consume = [list](const QJsonObject &item) { return Private::collect(*list, item); };
    pager->done = [promise, list](const Result<void> &result) {
        if (!result.ok()) {
            promise.finish(result.error());
            return;
        }
        promise.finish(*list);
    };
    d->page(pager, 0, slot);
    return promise.future();
}

// Looks pasted links up one after another. Kept alive only by the request in flight (and the first queued step), so
// an import whose client is destroyed simply ends: its promise goes, and the caller's future ends canceled.
class SpotifyClient::LinkImporter : public std::enable_shared_from_this<LinkImporter>
{
public:
    LinkImporter(Private *d, QList<spotify::Link> links, detail::SharedPromise<Result<LinkImport>> promise,
                 detail::CancelSlot slot)
        : m_d(d)
        , m_links(std::move(links))
        , m_promise(std::move(promise))
        , m_slot(std::move(slot))
    {}

    void next()
    {
        if (m_slot.isCancelled()) {
            return;
        }
        if (m_next >= m_links.size() || m_result.truncated) {
            finish(std::nullopt);
            return;
        }
        const spotify::Link link = m_links.at(m_next++);
        const auto self = shared_from_this();
        switch (link.kind) {
        case spotify::LinkKind::Track:
            m_d->get({u"/tracks/"_s + link.id, {}, kEntityTtl, {}, {}, Priority::Background}, m_slot,
                     [self, link](const Result<QJsonObject> &result) {
                         if (!result.ok()) {
                             self->settle(link, result.error());
                             return;
                         }
                         const Release release = detail::spotify::track(result.value());
                         if (release.title.isEmpty()) {
                             self->settle(link, apiError(ErrorCode::BadResponse, u"Spotify's answer has no track."_s));
                             return;
                         }
                         self->add({release});
                         self->settle(link, std::nullopt);
                     });
            return;
        case spotify::LinkKind::Album:
            m_d->albumWithTracks(link.id, Priority::Background, m_slot, [self, link](const Result<Album> &result) {
                if (!result.ok()) {
                    self->settle(link, result.error());
                    return;
                }
                self->add(result.value().tracks);
                self->settle(link, std::nullopt);
            });
            return;
        case spotify::LinkKind::Playlist:
            m_d->playlistTracks(link.id, Priority::Background, m_slot, [self, link](const Result<TrackList> &result) {
                if (!result.ok()) {
                    self->settle(link, result.error());
                    return;
                }
                self->add(result.value().tracks);
                self->m_result.skippedItems += result.value().skipped;
                self->settle(link, std::nullopt);
            });
            return;
        }
    }

private:
    // Errors after which no further link can be looked up.
    static bool stopsTheImport(const MetaError &error)
    {
        switch (error.code) {
        case ErrorCode::SignInRequired:
        case ErrorCode::MissingScope:
        case ErrorCode::QuotaExceeded:
        case ErrorCode::RateLimited:
        case ErrorCode::Refused:
        case ErrorCode::NotConfigured:
            return true;
        default:
            return false;
        }
    }

    void add(const QList<Release> &tracks)
    {
        for (const Release &release : tracks) {
            if (m_result.tracks.size() >= kMaxImportTracks) {
                m_result.truncated = true;
                return;
            }
            m_result.tracks.append(release);
        }
    }

    void settle(const spotify::Link &link, const std::optional<MetaError> &error)
    {
        if (error.has_value()) {
            m_result.failed.append({link, *error});
            if (stopsTheImport(*error)) {
                finish(error);
                return;
            }
        } else {
            ++m_resolved;
        }
        next();
    }

    // Ends the import; when an error stopped it, the links not tried yet fail with that error too.
    void finish(const std::optional<MetaError> &stop)
    {
        if (stop.has_value()) {
            for (qsizetype i = m_next; i < m_links.size(); ++i) {
                m_result.failed.append({m_links.at(i), *stop});
            }
            if (m_resolved == 0) {
                m_promise.finish(*stop);
                return;
            }
        }
        m_promise.finish(m_result);
    }

    Private *m_d;
    QList<spotify::Link> m_links;
    detail::SharedPromise<Result<LinkImport>> m_promise;
    detail::CancelSlot m_slot;
    LinkImport m_result;
    qsizetype m_next = 0;
    int m_resolved = 0;
};

QFuture<Result<LinkImport>> SpotifyClient::importLinks(const spotify::PastedLinks &links)
{
    const detail::SharedPromise<Result<LinkImport>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    if (links.links.isEmpty()) {
        promise.finishLater(this, LinkImport{});
        return promise.future();
    }
    const auto importer = std::make_shared<LinkImporter>(d.get(), links.links, promise, slot);
    QMetaObject::invokeMethod(this, [importer] { importer->next(); }, Qt::QueuedConnection);
    return promise.future();
}

} // namespace selecta::meta
