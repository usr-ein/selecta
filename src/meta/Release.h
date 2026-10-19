// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

// Value types that meta returns. All are plain copyable values: reentrant, safe to copy across threads, no
// ownership. Text comes from the service unchanged (Spotify's "Title - Extended Mix" stays as it is); nothing is
// invented when a service doesn't say: unknown numbers stay empty optionals, unknown text stays empty.
namespace selecta::meta {

// Where a release's core fields came from.
enum class Source : quint8 {
    Spotify,
    Deezer,
    MusicBrainz,
    CoverArtArchive,
    YouTube,
};

QString sourceName(Source source); // "Spotify", "Deezer", … Thread-safe (pure).

enum class DatePrecision : quint8 {
    Unknown,
    Year,  // "2017"
    Month, // "2017-06"
    Day,   // "2017-06-09"
};

// A cover image the service links to. Width and height are 0 when the service doesn't say.
struct CoverRef
{
    QUrl url;
    int width = 0;
    int height = 0;

    bool operator==(const CoverRef &) const = default;
};

// One track on one release: the "release" of the PRD glossary (D23), which the arrival pipeline tags from (META-4,
// D30) and the Soulseek ranker measures files against.
struct Release
{
    Source source = Source::Spotify;

    QString title;       // as the service writes it, version included
    QStringList artists; // credited order
    QString album;
    QStringList albumArtists; // credited order; Spotify's "Various Artists" kept as is
    QString albumType;        // lower case, as the service says: "album", "single", "compilation", "ep"
    QString releaseDate;      // ISO-8601 prefix matching releaseDatePrecision: "2017", "2017-06", "2017-06-09"
    DatePrecision releaseDatePrecision = DatePrecision::Unknown;
    std::optional<int> trackNumber;
    std::optional<int> trackTotal;
    std::optional<int> discNumber;
    std::optional<int> discTotal; // known only when the whole album was read (SpotifyClient::album)
    std::optional<qint64> durationMs;
    QString isrc;           // upper case, 12 characters, or empty
    QString upc;            // album barcode (UPC/EAN), or empty
    QList<CoverRef> covers; // largest first

    // Identifiers and link-backs (Spotify Developer Policy §II.4 asks for a link next to displayed data).
    QString spotifyId;
    QUrl spotifyUrl;
    QString spotifyAlbumId;
    QString deezerId;
    QUrl deezerUrl;
    QString deezerAlbumId;
    QString musicBrainzRecordingId;
    QString musicBrainzReleaseId;
    QString musicBrainzReleaseGroupId;

    // Label and genre: Spotify's development mode no longer returns them, so they are filled later (D30), from
    // MusicBrainz by ISRC, or from Deezer's album when Deezer is the source. The *Source fields say which.
    QString label;
    QString catalogNumber;
    QStringList genres; // most votes first
    std::optional<Source> labelSource;
    std::optional<Source> genreSource;
    QString copyrightP; // Spotify's ℗ line: a label hint for the inspector, never written as the label

    // ", "-joined, the ID3v2.3 convention (never "/": names like "AC/DC").
    QString artist() const { return artists.join(u", "); }
    QString albumArtist() const { return albumArtists.join(u", "); }

    bool operator==(const Release &) const = default;
};

// A playlist the signed-in user can import (own or collaborative, Spotify development mode).
struct PlaylistSummary
{
    QString id;
    QString name;
    QString owner; // display name
    int trackCount = 0;
    bool collaborative = false;
    QUrl spotifyUrl;
};

// The tracks of a playlist, Liked Songs or an album, ready to become Get tracks lines (META-3).
struct TrackList
{
    QString name;           // playlist or album name; "Liked Songs"
    QList<Release> tracks;  // in order; local files (no Spotify ID) carry title, artists and duration only
    int skipped = 0;        // items that aren't tracks (podcast episodes) or are unreadable
    bool truncated = false; // stopped at the size bound (SpotifyClient::kMaxImportTracks)
};

} // namespace selecta::meta

Q_DECLARE_METATYPE(selecta::meta::Release)
