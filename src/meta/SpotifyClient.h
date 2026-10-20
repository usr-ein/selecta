// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Priority.h"
#include "meta/Release.h"
#include "meta/Result.h"
#include "meta/SpotifyLinks.h"

#include <QFuture>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>
#include <utility>

class QNetworkAccessManager;

namespace selecta::meta {

class MetaCache;
class RequestPacer;
class SpotifyAuth;

// A track search by fields. Values are cleaned (quotes and control characters removed, spaces collapsed) and sent as
// Spotify field filters: track:"…" artist:"…" album:"…", with `version` words as free terms.
struct TrackQuery
{
    QString artist;                            // main artist
    QString title;                             // the title without its version
    QString version;                           // "Extended Mix", "Bicep Remix"…; optional
    QString album;                             // optional
    int limit = 10;                            // 1…10: development mode's maximum (§3.3)
    Priority priority = Priority::Interactive; // Background for Get tracks batches
};

// An album with every track, album-level fields copied into each track's Release (track and disc totals included).
// Album tracks are Spotify's simplified objects: they carry no ISRC (look a track up with track() for it).
struct Album
{
    QString id;
    QString name;
    QStringList artists;
    QString albumType;
    QString releaseDate;
    DatePrecision releaseDatePrecision = DatePrecision::Unknown;
    int totalTracks = 0;
    int discTotal = 0;
    QString upc;
    QString copyrightP;
    QList<CoverRef> covers;
    QUrl spotifyUrl;
    QList<Release> tracks;
    bool truncated = false; // more than SpotifyClient::kMaxImportTracks tracks
};

// What importLinks() made of pasted links: the tracks in paste order, and the links that failed.
struct LinkImport
{
    QList<Release> tracks;
    QList<std::pair<spotify::Link, MetaError>> failed;
    int skippedItems = 0; // items inside imported playlists that aren't tracks
    bool truncated = false;
};

// The Spotify Web API calls Selecta needs, limited to the endpoints development mode still serves (2026; see
// DESIGN.md "Spotify endpoints"): search, track, album (+ its tracks), the user's playlists, playlist items (own or
// collaborative playlists only), Liked Songs. Maps answers to Release without inventing anything: a field Spotify
// doesn't send stays empty.
//
// Every call needs a sign-in (SpotifyAuth); without one it fails with SignInRequired. Playlist calls need
// SpotifyAuth::playlistScopes() and Liked Songs SpotifyAuth::libraryScopes(); without them they fail with
// MissingScope. A 401 refreshes the access token once and retries; 429 waits for Retry-After when it is short, else
// fails with RateLimited (retryAfter set); 429 with reason QUOTA_EXCEEDED stops Spotify calls until Retry-After (1 h
// when absent) and fails with QuotaExceeded; 403 fails with Forbidden; malformed JSON with BadResponse.
//
// Caching (MetaCache, optional): searches 24 h, tracks and albums 30 days; the user's playlists and Liked Songs never.
// Entries are dropped when SpotifyAuth signs out.
//
// Threading: lives on the meta thread; futures finish there. Ownership: auth, network manager, pacer and cache must
// outlive the client; none is owned. Cancelling a returned future aborts its request.
class SpotifyClient : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxSearchLimit = 10;
    static constexpr int kMaxImportTracks = 10'000; // per playlist, album or paste
    static constexpr int kPageSize = 50;

    SpotifyClient(SpotifyAuth &auth, QNetworkAccessManager *network, RequestPacer &pacer, MetaCache *cache,
                  const base::Clock &clock, QObject *parent = nullptr,
                  QUrl apiUrl = QUrl(QStringLiteral("https://api.spotify.com/v1")));
    ~SpotifyClient() override;
    SpotifyClient(const SpotifyClient &) = delete;
    SpotifyClient &operator=(const SpotifyClient &) = delete;
    SpotifyClient(SpotifyClient &&) = delete;
    SpotifyClient &operator=(SpotifyClient &&) = delete;

    // Signed in (user or client credentials).
    bool isAvailable() const;

    // Up to query.limit (≤ 10) tracks. Fails with InvalidArgument when both artist and title are empty.
    QFuture<Result<QList<Release>>> searchTracks(const TrackQuery &query);
    // Tracks carrying this ISRC (q=isrc:…). Fails with InvalidArgument for a malformed ISRC; an empty list when
    // Spotify has none.
    QFuture<Result<QList<Release>>> searchIsrc(const QString &isrc, Priority priority = Priority::Interactive);
    // One track. NotFound when Spotify has no such ID; InvalidArgument for a malformed one.
    QFuture<Result<Release>> track(const QString &trackId, Priority priority = Priority::Interactive);
    // One album with all its tracks (paged), disc total computed from the tracks.
    QFuture<Result<Album>> album(const QString &albumId, Priority priority = Priority::Interactive);

    // The signed-in user's playlists (own and followed; only own or collaborative ones can be imported).
    QFuture<Result<QList<PlaylistSummary>>> myPlaylists();
    // A playlist's tracks. Forbidden for a playlist the user neither owns nor collaborates on (development mode):
    // paste its track links instead (importLinks()).
    QFuture<Result<TrackList>> playlistTracks(const QString &playlistId);
    // The user's Liked Songs, most recently liked first.
    QFuture<Result<TrackList>> likedSongs();
    // Resolves pasted links: one lookup per track link (no batch endpoint in development mode), album and playlist
    // links through album() and playlistTracks(), all in the Background lane. A failing link lands in
    // LinkImport::failed; the call itself fails only when nothing can be looked up at all (not signed in, quota, rate
    // limit).
    QFuture<Result<LinkImport>> importLinks(const spotify::PastedLinks &links);

    // The q parameter for a TrackQuery (exposed for tests).
    static QString searchQueryText(const TrackQuery &query);

private:
    struct Private;
    class LinkImporter;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
