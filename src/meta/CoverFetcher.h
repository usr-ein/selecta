// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Priority.h"
#include "meta/Release.h"
#include "meta/Result.h"

#include <QByteArray>
#include <QFuture>
#include <QObject>
#include <QString>
#include <QUrl>

#include <chrono>
#include <memory>

class QNetworkAccessManager;

namespace selecta::meta {

class RequestPacer;

// Cover bytes as downloaded. meta never decodes or re-encodes them: the convert library resizes them (CONV-5).
struct CoverImage
{
    QByteArray bytes;
    QString mimeType; // "image/jpeg" or "image/png"
    int width = 0;    // from the image header
    int height = 0;
    QUrl url; // where they came from
    Source source = Source::Spotify;
};

// Fetches the cover for a release (META-4, D29/D30): the largest image the release links to (Spotify's ~640 px, or
// Deezer's 1000 px; only from their CDNs: i.scdn.co, *.spotifycdn.com, *.dzcdn.net), then the Cover Art Archive front
// image by release MBID, then by release-group MBID (the 1200 px thumbnail, never the unbounded original). Each
// download is bounded: an answer announcing or reaching more than 20 MiB is aborted, and an image is accepted only
// when its header says JPEG or PNG with both sides ≤ 10 000 px (read from the bytes before anything decodes them).
//
// Covers are cached in `cacheDir` (one file per source URL plus a small JSON sidecar, kept `ttl`, oldest dropped
// beyond `maxCacheBytes`); the folder is read and written on a private worker thread, never on the caller's. A folder
// that can't be created just disables the cache.
//
// Threading: lives on the meta thread; futures finish there. Ownership: network manager and pacer must outlive the
// fetcher. Failures (after every source was tried): NotFound when the release links no cover and has no MBID,
// otherwise the last source's error (Network, TooLarge, UnsupportedFormat, BadResponse…). Cancelling the future stops
// at the current step.
class CoverFetcher : public QObject
{
    Q_OBJECT

public:
    struct Config
    {
        QString cacheDir; // empty: no disk cache
        std::chrono::hours ttl{24 * 30};
        qint64 maxCacheBytes = qint64{500} * 1024 * 1024;
        QUrl coverArtArchiveUrl{QStringLiteral("https://coverartarchive.org")}; // tests point it at a fake
    };

    CoverFetcher(Config config, QNetworkAccessManager *network, RequestPacer &pacer, const base::Clock &clock,
                 QObject *parent = nullptr);
    ~CoverFetcher() override;
    CoverFetcher(const CoverFetcher &) = delete;
    CoverFetcher &operator=(const CoverFetcher &) = delete;
    CoverFetcher(CoverFetcher &&) = delete;
    CoverFetcher &operator=(CoverFetcher &&) = delete;

    QFuture<Result<CoverImage>> fetch(const Release &release, Priority priority = Priority::Interactive);

    // True for the CDN hosts a release's cover URL may point to (HTTPS only). Thread-safe (pure).
    static bool isAllowedCoverUrl(const QUrl &url);

    // Checks downloaded bytes the way fetch() does: JPEG or PNG header, sides 1…10 000 px, at most 20 MiB. Fills
    // mimeType, width and height. Thread-safe (pure).
    static Result<CoverImage> inspect(QByteArray bytes, QUrl url, Source source);

private:
    struct Private;
    class Job;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
