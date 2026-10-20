// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/ClipSource.h"

#include "meta/DeezerClient.h"
#include "meta/ReleaseResolver.h"
#include "meta/detail/Async.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/TitleInfo.h"

#include <QMetaObject>
#include <QRegularExpression>

#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::meta {

namespace {

constexpr int kSearchLimit = 25;
constexpr qint64 kLengthWindowMs = 3000; // D23
constexpr double kMinimumScore = 0.80;   // a Likely match (ReleaseResolver's tiers)
constexpr std::chrono::minutes kFallbackLifetime{10};

MetaError notFound(const QString &message)
{
    return makeError(Service::Deezer, ErrorCode::NotFound, message);
}

bool isDeezerId(const QString &id)
{
    static const QRegularExpression pattern(uR"(^[1-9][0-9]{0,18}$)"_s);
    return pattern.match(id).hasMatch();
}

ResolveQuery queryFor(const Release &release)
{
    ResolveQuery query;
    query.artist = release.artists.join(u", "_s);
    query.title = release.title;
    query.album = release.album;
    query.durationMs = release.durationMs;
    query.isrc = release.isrc;
    return query;
}

bool lengthsAgree(const Release &a, const Release &b)
{
    if (!a.durationMs.has_value() || !b.durationMs.has_value() || *a.durationMs <= 0 || *b.durationMs <= 0) {
        return true; // nothing to compare
    }
    return std::abs(*a.durationMs - *b.durationMs) <= kLengthWindowMs;
}

// The search hit that is this release: version agreeing, length within ±3 s, at least a Likely score.
std::optional<DeezerTrack> bestHit(const Release &release, const QList<DeezerTrack> &hits)
{
    const ResolveQuery query = queryFor(release);
    std::optional<DeezerTrack> best;
    double bestScore = 0.0;
    for (const DeezerTrack &hit : hits) {
        const Candidate candidate = ReleaseResolver::score(query, hit.release);
        const bool versionAgrees = candidate.version == match::VersionCompatibility::Same ||
                                   candidate.version == match::VersionCompatibility::ProbablySame;
        if (!versionAgrees || !lengthsAgree(release, hit.release)) {
            continue;
        }
        const double score = candidate.confidence == Confidence::Exact ? 1.0 : candidate.score;
        if (score >= kMinimumScore && score > bestScore) {
            best = hit;
            bestScore = score;
        }
    }
    return best;
}

// One clip lookup, step by step: the Deezer ID, the ISRC, a search; then a fresh signed URL. Kept alive by the step in
// flight only.
class ClipLookup : public std::enable_shared_from_this<ClipLookup>
{
public:
    ClipLookup(QObject *context, DeezerClient &deezer, Release release, detail::SharedPromise<Result<Clip>> promise,
               detail::CancelSlot slot)
        : m_context(context)
        , m_deezer(deezer)
        , m_release(std::move(release))
        , m_promise(std::move(promise))
        , m_slot(std::move(slot))
    {}

    void start()
    {
        if (isDeezerId(m_release.deezerId)) {
            byId(m_release.deezerId, true);
            return;
        }
        byIsrc();
    }

private:
    template <typename T>
    void await(const QFuture<Result<T>> &future, std::function<void(const Result<T> &)> then)
    {
        m_slot.set([pending = QFuture<Result<T>>(future)]() mutable { pending.cancel(); });
        const auto self = shared_from_this();
        const auto callback = std::make_shared<std::function<void(const Result<T> &)>>(std::move(then));
        detail::whenReady(future, m_context, [self, callback](const Result<T> &result) {
            self->m_slot.clear();
            if (!self->m_slot.isCancelled()) {
                (*callback)(result);
            }
        });
    }

    // A fresh lookup of one track: its signed preview URL is only good for minutes. `fallBack`: a stale ID moves on
    // to the ISRC and the search instead of failing.
    void byId(const QString &id, bool fallBack)
    {
        await<DeezerTrack>(m_deezer.track(id, true, Priority::Interactive),
                           [this, fallBack](const Result<DeezerTrack> &result) {
                               if (result.ok()) {
                                   finishWith(result.value());
                               } else if (fallBack && result.error().code == ErrorCode::NotFound) {
                                   byIsrc();
                               } else {
                                   m_promise.finish(result.error());
                               }
                           });
    }

    void byIsrc()
    {
        const QString isrc = detail::normalizedIsrc(m_release.isrc);
        if (isrc.isEmpty()) {
            bySearch();
            return;
        }
        await<DeezerTrack>(m_deezer.trackByIsrc(isrc, true, Priority::Interactive),
                           [this](const Result<DeezerTrack> &result) {
                               if (result.ok()) {
                                   finishWith(result.value());
                               } else if (result.error().code == ErrorCode::NotFound) {
                                   bySearch();
                               } else {
                                   m_promise.finish(result.error());
                               }
                           });
    }

    void bySearch()
    {
        const QString artist =
            m_release.artists.isEmpty() ? QString() : detail::primaryArtist(m_release.artists.first());
        const detail::TitleInfo title = detail::analyseTitle(m_release.title);
        const QString text = QString(artist + u' ' + title.coreDisplay).trimmed();
        if (text.isEmpty()) {
            m_promise.finish(notFound(u"Nothing to look up on Deezer."_s));
            return;
        }
        await<QList<DeezerTrack>>(m_deezer.search(text, kSearchLimit, Priority::Interactive),
                                  [this](const Result<QList<DeezerTrack>> &result) {
                                      if (!result.ok()) {
                                          m_promise.finish(result.error());
                                          return;
                                      }
                                      const std::optional<DeezerTrack> hit = bestHit(m_release, result.value());
                                      if (!hit.has_value()) {
                                          m_promise.finish(notFound(u"Deezer has no clip of this version."_s));
                                          return;
                                      }
                                      byId(hit->release.deezerId, false); // search answers are cached: sign afresh
                                  });
    }

    void finishWith(const DeezerTrack &track)
    {
        if (track.previewUrl.isEmpty()) {
            m_promise.finish(notFound(u"Deezer has no clip for this track."_s));
            return;
        }
        Clip clip;
        clip.url = track.previewUrl;
        clip.expiresAt = DeezerClient::previewExpiry(
            track.previewUrl, QDateTime::currentDateTimeUtc().addSecs(std::chrono::seconds(kFallbackLifetime).count()));
        clip.pageUrl = track.release.deezerUrl;
        clip.source = Source::Deezer;
        clip.deezerTrackId = track.release.deezerId;
        m_promise.finish(clip);
    }

    QObject *m_context;
    DeezerClient &m_deezer;
    Release m_release;
    detail::SharedPromise<Result<Clip>> m_promise;
    detail::CancelSlot m_slot;
};

} // namespace

struct ClipSource::Private
{
    DeezerClient &deezer;
};

ClipSource::ClipSource(DeezerClient &deezer, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(Private{deezer}))
{}

ClipSource::~ClipSource() = default;

QFuture<Result<Clip>> ClipSource::clipFor(const Release &release)
{
    const detail::SharedPromise<Result<Clip>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const auto lookup = std::make_shared<ClipLookup>(this, d->deezer, release, promise, slot);
    QMetaObject::invokeMethod(this, [lookup] { lookup->start(); }, Qt::QueuedConnection);
    return promise.future();
}

} // namespace selecta::meta
