// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Release.h"
#include "meta/Result.h"

#include <QFuture>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace selecta::meta {

class MetaCache;
class RequestPacer;

namespace musicbrainz {

struct Recording
{
    QString id;
    QString title;
    std::optional<qint64> lengthMs;
    QStringList artists;   // credited names, in order
    QStringList artistIds; // MBIDs, same order
    int score = 0;         // search relevance 0…100 (searches only)
};

struct LabelInfo
{
    QString label;
    QString catalogNumber;
};

struct ReleaseInfo
{
    QString id;
    QString title;
    QString date;   // "2017", "2017-06" or "2017-06-09", or empty
    QString status; // "Official", "Bootleg", "Promotion"…
    QString barcode;
    QString releaseGroupId;
    QString primaryType;        // "Single", "EP", "Album"…
    QStringList secondaryTypes; // "Compilation", "DJ-mix", "Soundtrack"…
    QList<LabelInfo> labels;
};

struct Genre
{
    QString name;
    int count = 0; // votes
};

enum class Entity : quint8 {
    ReleaseGroup,
    Recording,
    Artist,
};

// What enrich() found for a release (D30's label and genre gap fill), with the reasons for the inspector.
struct Enrichment
{
    QString recordingId;
    QString releaseId;
    QString releaseGroupId;
    QString label; // empty when MusicBrainz has none
    QString catalogNumber;
    QStringList genres;  // most votes first; empty when nobody voted
    QStringList reasons; // "ISRC GBCFB1700229: 2 recordings, 1 within ±3 s", "release chosen by barcode"…
};

} // namespace musicbrainz

// MusicBrainz web service (references/metadata-and-preview-services.md §5.3, §10.1): recordings by ISRC, their
// releases with labels and catalogue numbers, and community genres. Every request carries the User-Agent
// "Selecta/<version> ( https://github.com/usr-ein/selecta )" and requests start ≥ 1.1 s apart (1 per second on
// average, kMusicBrainzPolicy); a 503 is routine and retried after 2 s, doubling, at most 3 tries in all.
//
// Caching (MetaCache, optional): lookups 30 days, an ISRC MusicBrainz doesn't know 24 h, searches 24 h.
//
// Threading: lives on the meta thread; futures finish there. Ownership: network manager, pacer and cache must outlive
// the client. Cancelling a returned future aborts its request. Failures: NotFound for an unknown ISRC or MBID,
// InvalidArgument for a malformed one, RateLimited / ServerError when 503s persist, BadResponse for malformed JSON.
class MusicBrainzClient : public QObject
{
    Q_OBJECT

public:
    static QByteArray userAgent(); // "Selecta/<version> ( https://github.com/usr-ein/selecta )"

    MusicBrainzClient(QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache, const base::Clock &clock,
                      QObject *parent = nullptr, QUrl apiUrl = QUrl(QStringLiteral("https://musicbrainz.org/ws/2")));
    ~MusicBrainzClient() override;
    MusicBrainzClient(const MusicBrainzClient &) = delete;
    MusicBrainzClient &operator=(const MusicBrainzClient &) = delete;
    MusicBrainzClient(MusicBrainzClient &&) = delete;
    MusicBrainzClient &operator=(MusicBrainzClient &&) = delete;

    // Recordings carrying `isrc` (GET /isrc/{isrc}?inc=artist-credits). NotFound when MusicBrainz doesn't know it.
    QFuture<Result<QList<musicbrainz::Recording>>> recordingsByIsrc(const QString &isrc);
    // Releases of a recording with labels and release groups (GET /release?recording=…&inc=labels+release-groups).
    QFuture<Result<QList<musicbrainz::ReleaseInfo>>> releasesOfRecording(const QString &recordingId);
    // Community genres of an entity, most votes first (GET /<entity>/{mbid}?inc=genres).
    QFuture<Result<QList<musicbrainz::Genre>>> genres(musicbrainz::Entity entity, const QString &id);
    // Recording search by title, artist and length ±3 s (Lucene query; special characters escaped).
    QFuture<Result<QList<musicbrainz::Recording>>> searchRecordings(const QString &title, const QString &artist,
                                                                    std::optional<qint64> durationMs);

    // D30 gap fill for a release (§5.3): recording by ISRC (or, when MusicBrainz doesn't know the ISRC, one search
    // accepted only at score ≥ 90 with a matching artist), keeping recordings within ±3 s of the release's length;
    // then its releases, choosing by barcode = UPC, else title and date, else the earliest official release of the
    // same type, never a compilation or DJ mix unless the release is one; then genres from the release group, the
    // recording or the artist. About 3 requests at 1/s. NotFound when no recording fits.
    QFuture<Result<musicbrainz::Enrichment>> enrich(const Release &release);

    // Copies an enrichment into a release (label, catalogue number, genres, MBIDs; labelSource/genreSource =
    // MusicBrainz when filled). Thread-safe (pure).
    static Release applied(Release release, const musicbrainz::Enrichment &enrichment);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
