// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Priority.h"
#include "meta/Release.h"
#include "meta/Result.h"

#include <QDateTime>
#include <QFuture>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace selecta::meta {

class MetaCache;
class RequestPacer;

// A Deezer track: the release it describes plus what only Deezer has.
struct DeezerTrack
{
    Release release;           // source Deezer; title includes the version ("Glue (Mixed)")
    QString titleVersion;      // Deezer's separate version text, "(Mixed)", or empty
    QUrl previewUrl;           // 30-s MP3 clip; signed, expires about 15 minutes after the answer
    std::optional<double> bpm; // Deezer's own analysis; often absent (0 is reported as absent)
};

// A Deezer album: label, genres and barcode, which Spotify's development mode no longer gives.
struct DeezerAlbum
{
    QString id;
    QString title;
    QString label;
    QString upc;
    QStringList genres;
    QString recordType; // "single", "ep", "album", "compile"
    QString releaseDate;
    int trackCount = 0;
    QList<CoverRef> covers; // largest first (cover_xl is 1000 px)
    QUrl url;
};

// Deezer's keyless "Simple API" (§9.1): the keyless metadata source when Spotify isn't signed in or has no match, and
// the source of official 30-second clips (ClipSource). Plain-text search only: the documented advanced syntax returned
// nothing in tests (§9.1), so it isn't used. Errors that Deezer reports inside an HTTP 200 body are mapped: code 4
// (quota) waits 5 s, doubling, then fails with RateLimited; 800 (no data) is NotFound.
//
// Caching (MetaCache, optional): searches 24 h, tracks and albums 30 days. Preview URLs expire, so a cached answer is
// used for metadata only: trackByIsrc() and track() always ask Deezer for a fresh preview URL (`fresh` = true).
//
// Threading: lives on the meta thread; futures finish there. Ownership: network manager, pacer and cache must outlive
// the client. Cancelling a returned future aborts its request.
class DeezerClient : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxSearchLimit = 50;

    DeezerClient(QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache, const base::Clock &clock,
                 QObject *parent = nullptr, QUrl apiUrl = QUrl(QStringLiteral("https://api.deezer.com")));
    ~DeezerClient() override;
    DeezerClient(const DeezerClient &) = delete;
    DeezerClient &operator=(const DeezerClient &) = delete;
    DeezerClient(DeezerClient &&) = delete;
    DeezerClient &operator=(DeezerClient &&) = delete;

    // Plain-text track search ("artist title"), up to `limit` (1…50) hits. InvalidArgument for an empty query.
    QFuture<Result<QList<DeezerTrack>>> search(const QString &query, int limit = 25,
                                               Priority priority = Priority::Interactive);
    // The track Deezer files under this ISRC (it picks one release itself). NotFound when Deezer has none.
    // `fresh` skips the cache (for a preview URL that must not have expired).
    QFuture<Result<DeezerTrack>> trackByIsrc(const QString &isrc, bool fresh = false,
                                             Priority priority = Priority::Interactive);
    // One track by Deezer's numeric ID. InvalidArgument for anything but digits; NotFound when it doesn't exist.
    QFuture<Result<DeezerTrack>> track(const QString &trackId, bool fresh = false,
                                       Priority priority = Priority::Interactive);
    QFuture<Result<DeezerAlbum>> album(const QString &albumId, Priority priority = Priority::Interactive);

    // The text search() sends for a query: control characters, quotes and colons (Deezer's advanced syntax, which
    // misbehaves) removed, spaces collapsed, at most 200 characters. Thread-safe (pure).
    static QString cleanQuery(const QString &query);

    // When a signed preview URL expires: the `exp=` time it carries, else `fallback` (Deezer's are ~15 min).
    // Thread-safe (pure).
    static QDateTime previewExpiry(const QUrl &previewUrl, const QDateTime &fallback);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
