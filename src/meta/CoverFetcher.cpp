// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/CoverFetcher.h"

#include "meta/ServicePolicy.h"
#include "meta/detail/Async.h"
#include "meta/detail/HttpChannel.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/ImageHeader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QSaveFile>
#include <QThreadPool>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::meta {

namespace {

Q_LOGGING_CATEGORY(lcMetaCovers, "selecta.meta.covers")

constexpr qint64 kMaxSidecarBytes = 4096;

MetaError coverError(Service service, ErrorCode code, QString message, int status = 0)
{
    return makeError(service, code, std::move(message), status);
}

// One place a cover may come from.
struct CoverSource
{
    QUrl url;
    Source source = Source::Spotify;
    Service service = Service::CoverImages;
};

QString cacheKey(const QUrl &url)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha256).toHex());
}

struct CachedCover
{
    QByteArray bytes;
    QUrl url;
    Source source = Source::Spotify;
};

// Reads a cached cover; drops it when expired, unreadable or oversized. Runs on the cache thread.
std::optional<CachedCover> readCached(const QString &dir, const QUrl &url, std::chrono::hours ttl)
{
    const QString base = QDir(dir).filePath(cacheKey(url));
    QFile sidecar(base + u".json"_s);
    QFile image(base + u".img"_s);
    const auto drop = [&] {
        QFile::remove(sidecar.fileName());
        QFile::remove(image.fileName());
        return std::nullopt;
    };
    if (!sidecar.exists() || !image.exists()) {
        return std::nullopt;
    }
    if (sidecar.size() > kMaxSidecarBytes || image.size() > kMaxCoverBytes || !sidecar.open(QIODevice::ReadOnly) ||
        !image.open(QIODevice::ReadOnly)) {
        return drop();
    }
    const QJsonObject meta = QJsonDocument::fromJson(sidecar.readAll()).object();
    const qint64 fetched = static_cast<qint64>(meta.value("fetched"_L1).toDouble(-1));
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - fetched;
    if (fetched <= 0 || age < 0 || age > std::chrono::duration_cast<std::chrono::milliseconds>(ttl).count() ||
        meta.value("url"_L1).toString() != url.toString(QUrl::FullyEncoded)) {
        return drop();
    }
    CachedCover cached;
    cached.bytes = image.read(kMaxCoverBytes + 1);
    cached.url = url;
    cached.source =
        static_cast<Source>(std::clamp(meta.value("source"_L1).toInt(), 0, static_cast<int>(Source::YouTube)));
    if (cached.bytes.size() > kMaxCoverBytes || cached.bytes.isEmpty()) {
        return drop();
    }
    return cached;
}

// Writes a cover and its sidecar atomically, then keeps the folder under `maxBytes` (oldest first). Cache thread.
void writeCached(const QString &dir, const CoverImage &cover, qint64 maxBytes)
{
    const QString base = QDir(dir).filePath(cacheKey(cover.url));
    QSaveFile image(base + u".img"_s);
    QSaveFile sidecar(base + u".json"_s);
    QJsonObject meta;
    meta.insert("url"_L1, cover.url.toString(QUrl::FullyEncoded));
    meta.insert("source"_L1, static_cast<int>(cover.source));
    meta.insert("fetched"_L1, static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    if (!image.open(QIODevice::WriteOnly) || image.write(cover.bytes) != cover.bytes.size() || !image.commit() ||
        !sidecar.open(QIODevice::WriteOnly) || sidecar.write(QJsonDocument(meta).toJson(QJsonDocument::Compact)) < 0 ||
        !sidecar.commit()) {
        qCWarning(lcMetaCovers) << "couldn't write a cover to the cache folder" << dir;
        QFile::remove(base + u".img"_s);
        QFile::remove(base + u".json"_s);
        return;
    }
    QFileInfoList files = QDir(dir).entryInfoList({u"*.img"_s}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo &file : std::as_const(files)) {
        total += file.size();
    }
    for (const QFileInfo &file : std::as_const(files)) { // oldest first
        if (total <= maxBytes) {
            break;
        }
        total -= file.size();
        QFile::remove(file.absoluteFilePath());
        QFile::remove(file.absolutePath() + u'/' + file.completeBaseName() + u".json"_s);
    }
}

} // namespace

struct CoverFetcher::Private
{
    CoverFetcher *q;
    Config config;
    std::unique_ptr<detail::HttpChannel> images;
    std::unique_ptr<detail::HttpChannel> archive;
    QThreadPool cacheThread;
    bool cacheUsable = false;

    Private(CoverFetcher *owner, Config c, QNetworkAccessManager *network, RequestPacer &pacer,
            const base::Clock &clock)
        : q(owner)
        , config(std::move(c))
        , images(std::make_unique<detail::HttpChannel>(Service::CoverImages, network, pacer, nullptr, clock))
        , archive(std::make_unique<detail::HttpChannel>(Service::CoverArtArchive, network, pacer, nullptr, clock))
    {
        cacheThread.setMaxThreadCount(1); // every cache file operation in order, off the caller's thread
        cacheThread.setObjectName(u"selecta-cover-cache"_s);
        if (!config.cacheDir.isEmpty()) {
            cacheUsable = QDir().mkpath(config.cacheDir);
            if (!cacheUsable) {
                qCWarning(lcMetaCovers) << "cover cache disabled: can't create" << config.cacheDir;
            }
        }
    }

    ~Private()
    {
        cacheThread.clear();
        cacheThread.waitForDone();
    }
    Private(const Private &) = delete;
    Private &operator=(const Private &) = delete;
    Private(Private &&) = delete;
    Private &operator=(Private &&) = delete;

    QList<CoverSource> sourcesFor(const Release &release) const
    {
        QList<CoverSource> sources;
        if (!release.covers.isEmpty() && isAllowedCoverUrl(release.covers.first().url)) {
            const QUrl url = release.covers.first().url;
            const bool deezer = url.host().toLower().endsWith(u"dzcdn.net"_s);
            sources.append({url, deezer ? Source::Deezer : Source::Spotify, Service::CoverImages});
        }
        const auto archiveUrl = [this](const QString &path) {
            QUrl url = config.coverArtArchiveUrl;
            url.setPath(url.path() + path);
            return url;
        };
        if (detail::isValidMbid(release.musicBrainzReleaseId)) {
            sources.append({archiveUrl(u"/release/"_s + release.musicBrainzReleaseId + u"/front-1200"_s),
                            Source::CoverArtArchive, Service::CoverArtArchive});
        }
        if (detail::isValidMbid(release.musicBrainzReleaseGroupId)) {
            sources.append({archiveUrl(u"/release-group/"_s + release.musicBrainzReleaseGroupId + u"/front-1200"_s),
                            Source::CoverArtArchive, Service::CoverArtArchive});
        }
        return sources;
    }
};

// Tries the sources one after another. Kept alive by the step in flight only.
class CoverFetcher::Job : public std::enable_shared_from_this<Job>
{
public:
    Job(Private *d, QList<CoverSource> sources, Priority priority, detail::SharedPromise<Result<CoverImage>> promise,
        detail::CancelSlot slot)
        : m_d(d)
        , m_sources(std::move(sources))
        , m_priority(priority)
        , m_promise(std::move(promise))
        , m_slot(std::move(slot))
    {}

    void next()
    {
        if (m_slot.isCancelled()) {
            return;
        }
        if (m_index >= m_sources.size()) {
            m_promise.finish(m_lastError.value_or(
                coverError(Service::CoverImages, ErrorCode::NotFound,
                           u"No cover: the release links none and MusicBrainz doesn't know its release."_s)));
            return;
        }
        const CoverSource source = m_sources.at(m_index++);
        if (!m_d->cacheUsable) {
            download(source);
            return;
        }
        const QString dir = m_d->config.cacheDir;
        const std::chrono::hours ttl = m_d->config.ttl;
        const detail::SharedPromise<std::optional<CachedCover>> read;
        m_d->cacheThread.start([read, dir, url = source.url, ttl] { read.finish(readCached(dir, url, ttl)); });
        const auto self = shared_from_this();
        detail::whenReady(read.future(), m_d->q, [self, source](const std::optional<CachedCover> &cached) {
            if (self->m_slot.isCancelled()) {
                return;
            }
            if (cached.has_value()) {
                Result<CoverImage> image = inspect(cached->bytes, cached->url, cached->source);
                if (image.ok()) {
                    self->m_promise.finish(std::move(image));
                    return;
                }
            }
            self->download(source);
        });
    }

private:
    void download(const CoverSource &source)
    {
        detail::HttpRequest request;
        request.url = source.url;
        request.priority = m_priority;
        request.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("image/jpeg, image/png;q=0.9")});
        detail::HttpChannel &channel = source.service == Service::CoverArtArchive ? *m_d->archive : *m_d->images;
        const auto self = shared_from_this();
        const auto id = channel.send(std::move(request), [self, source](const Result<detail::HttpResponse> &result) {
            self->m_slot.clear();
            self->onDownloaded(source, result);
        });
        m_slot.set([channel = &channel, id] { channel->cancel(id); });
    }

    void onDownloaded(const CoverSource &source, const Result<detail::HttpResponse> &result)
    {
        if (!result.ok()) {
            m_lastError = result.error();
            next();
            return;
        }
        const detail::HttpResponse &response = result.value();
        if (response.status != 200) {
            m_lastError = response.status == 404
                              ? coverError(source.service, ErrorCode::NotFound, u"No cover at this address."_s, 404)
                              : detail::statusError(source.service, response);
            next();
            return;
        }
        Result<CoverImage> image = inspect(response.body, source.url, source.source);
        if (!image.ok()) {
            MetaError error = image.error();
            error.service = source.service;
            m_lastError = error;
            next();
            return;
        }
        if (m_d->cacheUsable) {
            const QString dir = m_d->config.cacheDir;
            const qint64 maxBytes = m_d->config.maxCacheBytes;
            m_d->cacheThread.start([dir, cover = image.value(), maxBytes] { writeCached(dir, cover, maxBytes); });
        }
        m_promise.finish(std::move(image));
    }

    Private *m_d;
    QList<CoverSource> m_sources;
    qsizetype m_index = 0;
    Priority m_priority;
    detail::SharedPromise<Result<CoverImage>> m_promise;
    detail::CancelSlot m_slot;
    std::optional<MetaError> m_lastError;
};

CoverFetcher::CoverFetcher(Config config, QNetworkAccessManager *network, RequestPacer &pacer, const base::Clock &clock,
                           QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, std::move(config), network, pacer, clock))
{}

CoverFetcher::~CoverFetcher() = default;

bool CoverFetcher::isAllowedCoverUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != u"https"_s) {
        return false;
    }
    const QString host = url.host().toLower();
    static const std::array<QString, 3> kHosts = {u"scdn.co"_s, u"spotifycdn.com"_s, u"dzcdn.net"_s};
    return std::ranges::any_of(kHosts,
                               [&](const QString &suffix) { return host == suffix || host.endsWith(u'.' + suffix); });
}

Result<CoverImage> CoverFetcher::inspect(QByteArray bytes, QUrl url, Source source)
{
    if (bytes.size() > kMaxCoverBytes) {
        return coverError(Service::CoverImages, ErrorCode::TooLarge, u"The cover is larger than 20 MiB."_s);
    }
    const std::optional<detail::ImageInfo> info = detail::readImageHeader(bytes);
    if (!info.has_value()) {
        return coverError(Service::CoverImages, ErrorCode::UnsupportedFormat,
                          u"The cover isn't a readable JPEG or PNG image."_s);
    }
    if (info->width > static_cast<quint32>(kMaxCoverPixels) || info->height > static_cast<quint32>(kMaxCoverPixels)) {
        return coverError(Service::CoverImages, ErrorCode::TooLarge,
                          u"The cover is %1 × %2 px; covers over %3 px are refused."_s.arg(info->width)
                              .arg(info->height)
                              .arg(kMaxCoverPixels));
    }
    CoverImage image;
    image.bytes = std::move(bytes);
    image.mimeType = info->format == detail::ImageFormat::Jpeg ? u"image/jpeg"_s : u"image/png"_s;
    image.width = static_cast<int>(info->width);
    image.height = static_cast<int>(info->height);
    image.url = std::move(url);
    image.source = source;
    return image;
}

QFuture<Result<CoverImage>> CoverFetcher::fetch(const Release &release, Priority priority)
{
    const detail::SharedPromise<Result<CoverImage>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    const auto job = std::make_shared<Job>(d.get(), d->sourcesFor(release), priority, promise, slot);
    QMetaObject::invokeMethod(this, [job] { job->next(); }, Qt::QueuedConnection);
    return promise.future();
}

} // namespace selecta::meta
