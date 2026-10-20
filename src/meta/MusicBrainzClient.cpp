// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/MusicBrainzClient.h"

#include "meta/MetaCache.h"
#include "meta/ServicePolicy.h"
#include "meta/detail/Async.h"
#include "meta/detail/HttpChannel.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/Json.h"
#include "meta/detail/MusicBrainzJson.h"

#include <QJsonObject>
#include <QMetaObject>
#include <QUrlQuery>

#include <functional>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::meta {

namespace mbjson = detail::musicbrainz;

namespace {

constexpr qsizetype kMaxQueryField = 200;
constexpr qint64 kDurationWindowMs = 3000;

MetaError mbError(ErrorCode code, QString message, int status = 0)
{
    return makeError(Service::MusicBrainz, code, std::move(message), status);
}

// A value inside a Lucene phrase: only the backslash and the quote need escaping there.
QString phrase(const QString &text)
{
    QString value = detail::json::cleanText(text, kMaxQueryField);
    value.replace(u'\\', u"\\\\"_s);
    value.replace(u'"', u"\\\""_s);
    return u'"' + value + u'"';
}

// The title without a trailing version ("Glue - Extended Mix", "Glue (Extended Mix)" → "Glue"): MusicBrainz writes
// versions differently, and the length window tells the versions apart anyway.
QString baseTitle(const QString &title)
{
    QString base = title;
    if (const qsizetype dash = base.indexOf(u" - "_s); dash > 0) {
        base.truncate(dash);
    }
    if (base.endsWith(u')')) {
        if (const qsizetype open = base.lastIndexOf(u'('); open > 0) {
            base.truncate(open);
        }
    }
    return base.trimmed();
}

QString entityName(musicbrainz::Entity entity)
{
    switch (entity) {
    case musicbrainz::Entity::ReleaseGroup:
        return u"release group"_s;
    case musicbrainz::Entity::Recording:
        return u"recording"_s;
    case musicbrainz::Entity::Artist:
        return u"artist"_s;
    }
    return u"recording"_s;
}

QString entityPath(musicbrainz::Entity entity)
{
    switch (entity) {
    case musicbrainz::Entity::ReleaseGroup:
        return u"/release-group/"_s;
    case musicbrainz::Entity::Recording:
        return u"/recording/"_s;
    case musicbrainz::Entity::Artist:
        return u"/artist/"_s;
    }
    return u"/recording/"_s;
}

} // namespace

struct MusicBrainzClient::Private
{
    using JsonDone = std::function<void(Result<QJsonObject>)>;

    struct Get
    {
        QString path;
        QUrlQuery query;
        std::chrono::seconds ttl{0};
        std::chrono::seconds notFoundTtl{0};
        Priority priority = Priority::Background;
    };

    MusicBrainzClient *q;
    MetaCache *cache;
    QUrl api;
    std::unique_ptr<detail::HttpChannel> channel;

    Private(MusicBrainzClient *owner, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *c,
            const base::Clock &clock, QUrl apiUrl)
        : q(owner)
        , cache(c)
        , api(std::move(apiUrl))
        , channel(std::make_unique<detail::HttpChannel>(Service::MusicBrainz, network, pacer, c, clock))
    {
        channel->setInspector([](const detail::HttpResponse &response) {
            return detail::jsonInspection(Service::MusicBrainz, response);
        });
    }

    // A GET with fmt=json. `done` runs exactly once, never inside this call, unless `slot` is cancelled first.
    void get(const Get &request, const detail::CancelSlot &slot, const JsonDone &done)
    {
        if (slot.isCancelled()) {
            return;
        }
        QUrlQuery query = request.query;
        query.addQueryItem(u"fmt"_s, u"json"_s);
        QUrl url = api;
        url.setPath(api.path() + request.path);
        url.setQuery(query);
        detail::HttpRequest http;
        http.url = std::move(url);
        http.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")});
        http.cacheKey = u"musicbrainz"_s + request.path + u'?' + query.query(QUrl::FullyEncoded);
        http.cacheTtl = request.ttl;
        http.notFoundTtl = request.notFoundTtl;
        http.priority = request.priority;
        const auto callback = std::make_shared<JsonDone>(done);
        const auto id = channel->send(std::move(http), [callback, slot](const Result<detail::HttpResponse> &result) {
            slot.clear();
            if (!result.ok()) {
                (*callback)(result.error());
                return;
            }
            const detail::HttpResponse &response = result.value();
            if (response.status == 404) {
                (*callback)(mbError(ErrorCode::NotFound, u"MusicBrainz has no such entry."_s, 404));
                return;
            }
            if (response.status != 200) {
                (*callback)(detail::statusError(Service::MusicBrainz, response));
                return;
            }
            (*callback)(detail::json::parseObject(response.body, Service::MusicBrainz));
        });
        slot.set([this, id] { channel->cancel(id); });
    }

    template <typename T>
    QFuture<Result<T>> run(const Get &request, const std::function<Result<T>(const QJsonObject &)> &map)
    {
        const detail::SharedPromise<Result<T>> promise;
        const detail::CancelSlot slot;
        promise.onCanceled(q, [slot] { slot.cancel(); });
        get(request, slot, [promise, map](const Result<QJsonObject> &result) {
            if (!result.ok()) {
                promise.finish(result.error());
                return;
            }
            promise.finish(map(result.value()));
        });
        return promise.future();
    }
};

// The D30 gap fill for one release (§5.3), one request after another. Kept alive by the request in flight only.
class MusicBrainzClient::Enricher : public std::enable_shared_from_this<Enricher>
{
public:
    Enricher(Private *d, Release release, Priority priority,
             detail::SharedPromise<Result<musicbrainz::Enrichment>> promise, detail::CancelSlot slot)
        : m_d(d)
        , m_release(std::move(release))
        , m_priority(priority)
        , m_promise(std::move(promise))
        , m_slot(std::move(slot))
    {}

    void start()
    {
        const QString isrc = detail::normalizedIsrc(m_release.isrc);
        if (isrc.isEmpty()) {
            m_result.reasons.append(u"no ISRC: searching by title, artist and length"_s);
            search();
            return;
        }
        QUrlQuery query;
        query.addQueryItem(u"inc"_s, u"artist-credits"_s);
        const auto self = shared_from_this();
        m_d->get({u"/isrc/"_s + isrc, query, kEntityTtl, kNotFoundTtl, m_priority}, m_slot,
                 [self, isrc](const Result<QJsonObject> &result) {
                     if (!result.ok()) {
                         if (result.error().code == ErrorCode::NotFound) {
                             self->m_result.reasons.append(u"ISRC %1 unknown to MusicBrainz"_s.arg(isrc));
                             self->search();
                         } else {
                             self->m_promise.finish(result.error());
                         }
                         return;
                     }
                     const QList<musicbrainz::Recording> found = mbjson::recordings(result.value());
                     self->m_result.reasons.append(u"ISRC %1: %2 recording(s)"_s.arg(isrc).arg(found.size()));
                     self->chooseRecording(found);
                 });
    }

private:
    void search()
    {
        const QString title = baseTitle(m_release.title);
        if (title.isEmpty() || m_release.artists.isEmpty()) {
            m_promise.finish(mbError(ErrorCode::NotFound, u"MusicBrainz has no recording for this release."_s));
            return;
        }
        QUrlQuery query;
        query.addQueryItem(u"query"_s, recordingQuery(title, m_release.artists.first(), m_release.durationMs));
        query.addQueryItem(u"limit"_s, u"10"_s);
        const auto self = shared_from_this();
        m_d->get({u"/recording"_s, query, kSearchTtl, 0s, m_priority}, m_slot,
                 [self](const Result<QJsonObject> &result) {
                     if (!result.ok()) {
                         self->m_promise.finish(result.error());
                         return;
                     }
                     QList<musicbrainz::Recording> strong;
                     const QList<musicbrainz::Recording> all = mbjson::recordings(result.value());
                     for (const musicbrainz::Recording &recording : all) {
                         if (recording.score >= mbjson::kMinSearchScore) {
                             strong.append(recording);
                         }
                     }
                     self->m_result.reasons.append(u"search: %1 hit(s), %2 scoring ≥ %3"_s.arg(all.size())
                                                       .arg(strong.size())
                                                       .arg(mbjson::kMinSearchScore));
                     self->chooseRecording(strong);
                 });
    }

    void chooseRecording(const QList<musicbrainz::Recording> &candidates)
    {
        const QList<musicbrainz::Recording> fitting = mbjson::fittingRecordings(candidates, m_release);
        if (fitting.isEmpty()) {
            m_result.reasons.append(u"no recording by the artist within ±3 s"_s);
            m_promise.finish(mbError(ErrorCode::NotFound, m_result.reasons.join(u"; "_s)));
            return;
        }
        const musicbrainz::Recording &recording = fitting.first();
        m_result.recordingId = recording.id;
        for (const QString &artistId : recording.artistIds) {
            if (!artistId.isEmpty()) {
                m_artistId = artistId;
                break;
            }
        }
        m_result.reasons.append(u"recording \"%1\"%2"_s.arg(
            recording.title, fitting.size() > 1 ? u" (closest of %1 fitting)"_s.arg(fitting.size()) : QString()));
        QUrlQuery query;
        query.addQueryItem(u"recording"_s, recording.id);
        query.addQueryItem(u"inc"_s, u"labels+release-groups"_s);
        query.addQueryItem(u"limit"_s, u"100"_s);
        const auto self = shared_from_this();
        m_d->get({u"/release"_s, query, kEntityTtl, 0s, m_priority}, m_slot, [self](const Result<QJsonObject> &result) {
            if (!result.ok() && result.error().code != ErrorCode::NotFound) {
                self->m_promise.finish(result.error());
                return;
            }
            const QList<musicbrainz::ReleaseInfo> found =
                result.ok() ? mbjson::releases(result.value()) : QList<musicbrainz::ReleaseInfo>{};
            QString why;
            const std::optional<musicbrainz::ReleaseInfo> chosen = mbjson::chooseRelease(found, self->m_release, &why);
            self->m_result.reasons.append(why);
            if (chosen.has_value()) {
                self->m_result.releaseId = chosen->id;
                self->m_result.releaseGroupId = chosen->releaseGroupId;
                const musicbrainz::LabelInfo label = mbjson::labelOf(*chosen);
                self->m_result.label = label.label;
                self->m_result.catalogNumber = label.catalogNumber;
                if (label.label.isEmpty()) {
                    self->m_result.reasons.append(u"the release names no label"_s);
                }
            }
            self->nextGenres(self->m_result.releaseGroupId.isEmpty() ? musicbrainz::Entity::Recording
                                                                     : musicbrainz::Entity::ReleaseGroup);
        });
    }

    // Genres from the release group, else the recording, else the artist (§5.3); none is a fine answer.
    void nextGenres(musicbrainz::Entity entity)
    {
        QString id;
        switch (entity) {
        case musicbrainz::Entity::ReleaseGroup:
            id = m_result.releaseGroupId;
            break;
        case musicbrainz::Entity::Recording:
            id = m_result.recordingId;
            break;
        case musicbrainz::Entity::Artist:
            id = m_artistId;
            break;
        }
        if (id.isEmpty()) {
            m_promise.finish(m_result);
            return;
        }
        QUrlQuery query;
        query.addQueryItem(u"inc"_s, u"genres"_s);
        const auto self = shared_from_this();
        m_d->get({entityPath(entity) + id, query, kEntityTtl, 0s, m_priority}, m_slot,
                 [self, entity](const Result<QJsonObject> &result) {
                     if (!result.ok() && result.error().code != ErrorCode::NotFound) {
                         // The label is worth keeping: report the missing genres instead of failing.
                         self->m_result.reasons.append(u"genres unavailable: %1"_s.arg(result.error().message));
                         self->m_promise.finish(self->m_result);
                         return;
                     }
                     const QList<musicbrainz::Genre> found =
                         result.ok() ? mbjson::genres(result.value()) : QList<musicbrainz::Genre>{};
                     if (!found.isEmpty()) {
                         for (const musicbrainz::Genre &genre : found) {
                             self->m_result.genres.append(genre.name);
                         }
                         self->m_result.reasons.append(u"genres from the %1"_s.arg(entityName(entity)));
                         self->m_promise.finish(self->m_result);
                         return;
                     }
                     if (entity == musicbrainz::Entity::ReleaseGroup) {
                         self->nextGenres(musicbrainz::Entity::Recording);
                     } else if (entity == musicbrainz::Entity::Recording) {
                         self->nextGenres(musicbrainz::Entity::Artist);
                     } else {
                         self->m_result.reasons.append(u"no genre votes"_s);
                         self->m_promise.finish(self->m_result);
                     }
                 });
    }

    Private *m_d;
    Release m_release;
    Priority m_priority;
    detail::SharedPromise<Result<musicbrainz::Enrichment>> m_promise;
    detail::CancelSlot m_slot;
    musicbrainz::Enrichment m_result;
    QString m_artistId;
};

QByteArray MusicBrainzClient::userAgent()
{
    return detail::userAgent();
}

MusicBrainzClient::MusicBrainzClient(QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
                                     const base::Clock &clock, QObject *parent, QUrl apiUrl)
    : QObject(parent)
    , d(std::make_unique<Private>(this, network, pacer, cache, clock, std::move(apiUrl)))
{}

MusicBrainzClient::~MusicBrainzClient() = default;

QString MusicBrainzClient::recordingQuery(const QString &title, const QString &artist, std::optional<qint64> durationMs)
{
    QString query = u"recording:%1 AND artist:%2"_s.arg(phrase(title), phrase(artist));
    if (durationMs.has_value() && *durationMs > kDurationWindowMs) {
        query += u" AND dur:[%1 TO %2]"_s.arg(*durationMs - kDurationWindowMs).arg(*durationMs + kDurationWindowMs);
    }
    return query;
}

QFuture<Result<QList<musicbrainz::Recording>>> MusicBrainzClient::recordingsByIsrc(const QString &isrc,
                                                                                   Priority priority)
{
    const QString normalized = detail::normalizedIsrc(isrc);
    if (normalized.isEmpty()) {
        return detail::deferred<Result<QList<musicbrainz::Recording>>>(
            this, mbError(ErrorCode::InvalidArgument, u"\"%1\" isn't an ISRC."_s.arg(isrc.left(40))));
    }
    QUrlQuery query;
    query.addQueryItem(u"inc"_s, u"artist-credits"_s);
    return d->run<QList<musicbrainz::Recording>>({u"/isrc/"_s + normalized, query, kEntityTtl, kNotFoundTtl, priority},
                                                 [](const QJsonObject &object) { return mbjson::recordings(object); });
}

QFuture<Result<QList<musicbrainz::ReleaseInfo>>> MusicBrainzClient::releasesOfRecording(const QString &recordingId,
                                                                                        Priority priority)
{
    if (!detail::isValidMbid(recordingId)) {
        return detail::deferred<Result<QList<musicbrainz::ReleaseInfo>>>(
            this, mbError(ErrorCode::InvalidArgument, u"That isn't a MusicBrainz ID."_s));
    }
    QUrlQuery query;
    query.addQueryItem(u"recording"_s, recordingId);
    query.addQueryItem(u"inc"_s, u"labels+release-groups"_s);
    query.addQueryItem(u"limit"_s, u"100"_s);
    return d->run<QList<musicbrainz::ReleaseInfo>>({u"/release"_s, query, kEntityTtl, 0s, priority},
                                                   [](const QJsonObject &object) { return mbjson::releases(object); });
}

QFuture<Result<QList<musicbrainz::Genre>>> MusicBrainzClient::genres(musicbrainz::Entity entity, const QString &id,
                                                                     Priority priority)
{
    if (!detail::isValidMbid(id)) {
        return detail::deferred<Result<QList<musicbrainz::Genre>>>(
            this, mbError(ErrorCode::InvalidArgument, u"That isn't a MusicBrainz ID."_s));
    }
    QUrlQuery query;
    query.addQueryItem(u"inc"_s, u"genres"_s);
    return d->run<QList<musicbrainz::Genre>>({entityPath(entity) + id, query, kEntityTtl, 0s, priority},
                                             [](const QJsonObject &object) { return mbjson::genres(object); });
}

QFuture<Result<QList<musicbrainz::Recording>>> MusicBrainzClient::searchRecordings(const QString &title,
                                                                                   const QString &artist,
                                                                                   std::optional<qint64> durationMs,
                                                                                   Priority priority)
{
    if (detail::json::cleanText(title).isEmpty() || detail::json::cleanText(artist).isEmpty()) {
        return detail::deferred<Result<QList<musicbrainz::Recording>>>(
            this, mbError(ErrorCode::InvalidArgument, u"A recording search needs a title and an artist."_s));
    }
    QUrlQuery query;
    query.addQueryItem(u"query"_s, recordingQuery(title, artist, durationMs));
    query.addQueryItem(u"limit"_s, u"10"_s);
    return d->run<QList<musicbrainz::Recording>>({u"/recording"_s, query, kSearchTtl, 0s, priority},
                                                 [](const QJsonObject &object) { return mbjson::recordings(object); });
}

QFuture<Result<musicbrainz::Enrichment>> MusicBrainzClient::enrich(const Release &release, Priority priority)
{
    const detail::SharedPromise<Result<musicbrainz::Enrichment>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const auto enricher = std::make_shared<Enricher>(d.get(), release, priority, promise, slot);
    QMetaObject::invokeMethod(this, [enricher] { enricher->start(); }, Qt::QueuedConnection);
    return promise.future();
}

Release MusicBrainzClient::applied(Release release, const musicbrainz::Enrichment &enrichment)
{
    if (!enrichment.recordingId.isEmpty()) {
        release.musicBrainzRecordingId = enrichment.recordingId;
    }
    if (!enrichment.releaseId.isEmpty()) {
        release.musicBrainzReleaseId = enrichment.releaseId;
    }
    if (!enrichment.releaseGroupId.isEmpty()) {
        release.musicBrainzReleaseGroupId = enrichment.releaseGroupId;
    }
    if (!enrichment.label.isEmpty()) {
        release.label = enrichment.label;
        release.catalogNumber = enrichment.catalogNumber;
        release.labelSource = Source::MusicBrainz;
    }
    if (!enrichment.genres.isEmpty()) {
        release.genres = enrichment.genres;
        release.genreSource = Source::MusicBrainz;
    }
    return release;
}

} // namespace selecta::meta
