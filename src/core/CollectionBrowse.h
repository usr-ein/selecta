// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringView>

#include <memory>
#include <optional>

// What the Collection view shows (P-010): the collection's tracks and albums with their quality tiers, and the cover
// wall's sections for each way of grouping them. Plain values and pure functions: any thread.
namespace selecta::core {

// --- Quality tiers ---------------------------------------------------------------------------------------------------

// How good a collection track is, for the quality grouping and for upgrades: a higher tier is better, and Unknown is
// below every other (no upgrade can be judged better than an unknown quality, and none is offered for one).
enum class QualityTier : quint8 {
    Unknown,  // a lossy file whose bitrate isn't known
    Below192, // under 192 kbps
    From192,  // 192–255 kbps
    From256,  // 256–319 kbps
    Top,      // 320 kbps, or a V0-class VBR MP3
    Lossless, // from lossless: a lossless file, or a file Selecta made from a lossless download
};

inline constexpr int k320MinimumKbps = 315; // a 320 kbps stream measured a little low still counts as one
inline constexpr int kV0MinimumKbps = 220;  // a VBR MP3 averaging this much is V0-class (match/Quality.h agrees)

// What Selecta knows of a track beyond its stream facts: what its download said (History).
struct TierEvidence
{
    bool fromLossless = false;           // Selecta made it from a lossless download
    std::optional<int> likelySourceKbps; // the spectrum check found it made from a lossy file of this bitrate
};

// The tier of a lossy stream of `kbps` (the average, for VBR): Unknown for 0 or less.
QualityTier tierOfBitrate(int kbps);
// The tier of a track. A lossless codec, or Selecta's conversion from a lossless download, is Lossless; a lossy file
// goes by its bitrate, a VBR MP3 of kV0MinimumKbps or more on average with 320. A spectrum verdict puts a file in the
// tier of the bitrate it was likely made from when that is lower (a fake 320, a FLAC made from an MP3); it never raises
// one.
QualityTier qualityTier(const match::AudioQuality &quality, TierEvidence evidence = {});
// Where the quality grouping puts a tier: worst first, Unknown last.
int tierDisplayOrder(QualityTier tier);
// The tier in words, for a section title: "Below 192 kbps", "192–255 kbps", "256–319 kbps", "320 kbps or V0",
// "From lossless", "Quality unknown".
QString tierName(QualityTier tier);
// Whether a History quality text ("FLAC 16/44.1", "WAV 24/96", "MP3 320") says the download was lossless.
bool isLosslessQualityText(QStringView quality);

// --- The listing -----------------------------------------------------------------------------------------------------

// One track of the collection: what the index knows of it, its album, its tier and when it came.
struct CollectionTrack
{
    qint64 id = 0; // collection_files.id: CollectionStore::find(), the MatchIndex track id
    QString path;  // as found on disk
    QString artist;
    QString title; // with its version text
    QString album;
    QString albumArtist;
    QString genre; // the first genre tagged; empty: none
    int year = 0;
    int trackNumber = 0;
    int discNumber = 0;
    qint64 durationMs = 0;
    qint64 sizeBytes = 0;
    qint64 modifiedMs = 0; // the file's last change: with its size, what tells another file at the same path
    qint64 addedMs = 0;    // when Selecta downloaded it (History), else when the file last changed; Unix time, ms
    match::AudioQuality quality;
    QualityTier tier = QualityTier::Unknown;
    quint64 albumId = 0; // its album (CollectionAlbum::id); 0: a loose track of the tracks folder
};

// One album of the albums folder: one subfolder, whatever its tracks' tags say (collection::albumsOf).
struct CollectionAlbum
{
    quint64 id = 0; // stable across scans
    QString folder; // the album's folder, a subfolder of the albums folder
    QString title;  // the most common album tag, else what the folder's name says
    QString artist; // the album artist; "Various Artists" for a compilation
    int year = 0;   // 0: unknown
    bool variousArtists = false;

    friend bool operator==(const CollectionAlbum &, const CollectionAlbum &) = default;
};

// Every fact the same, the quality's included.
bool operator==(const CollectionTrack &a, const CollectionTrack &b);

// The whole collection, as the view shows it: built with the index (CollectionService), shared read-only.
struct CollectionListing
{
    QList<CollectionTrack> tracks; // sorted by path
    QList<CollectionAlbum> albums; // sorted by folder

    const CollectionAlbum *album(quint64 id) const;
    // The index in `tracks` of the track with this id, or -1.
    qsizetype indexOf(qint64 trackId) const;

    friend bool operator==(const CollectionListing &, const CollectionListing &) = default;
};

// --- The cover wall ----------------------------------------------------------------------------------------------

// How the cover wall is grouped.
enum class CollectionOrder : quint8 {
    Added,   // by month downloaded, newest first
    Artist,  // by the artist's initial, A to Z
    Genre,   // by genre, A to Z; tracks without one last
    Quality, // by tier, worst first; unknown last
};

// A tile of the cover wall: a loose track, or the tracks of one album that fall in its section.
struct CollectionTile
{
    quint64 albumId = 0; // 0: a loose track
    QList<int> tracks;   // indexes into CollectionListing::tracks: an album's in disc and track order

    friend bool operator==(const CollectionTile &, const CollectionTile &) = default;
};

struct CollectionSection
{
    QString title; // "October 2026", "B", "Deep House", "Below 192 kbps"
    QList<CollectionTile> tiles;
    int trackCount = 0;

    friend bool operator==(const CollectionSection &, const CollectionSection &) = default;
};

// The cover wall's sections. Grouping works per track: each track goes in its section, then the tracks of one album in
// one section make one tile, so an album whose tracks differ (in genre, in quality) has a tile in each of their
// sections. The artist of an album's tracks is the album's artist, so a compilation stays whole.
// - Added: one section per month of addedMs in local time ("October 2026"), newest first, then "Date unknown"; tiles
//   newest first.
// - Artist: one section per initial (accents dropped, upper case; "#" for anything but a letter, last); tiles by
//   artist, then album or title.
// - Genre: one section per genre, compared without case or accents and titled as most of its tracks spell it, A to
//   Z, then "No genre"; tiles by artist, then album or title.
// - Quality: one section per tier, worst first, then "Quality unknown"; tiles by artist, then album or title.
// Names compare without case or accents. Empty sections are left out.
QList<CollectionSection> groupCollection(const CollectionListing &listing, CollectionOrder order);

// Whether `x` plays before `y` on their album: by disc, then number, the unnumbered tracks after the numbered ones of
// their disc, then by path.
bool playsBefore(const CollectionTrack &x, const CollectionTrack &y);

// How names are compared and sorted: accents dropped, case folded, whitespace simplified ("Étienne de Crécy" →
// "etienne de crecy").
QString browseKey(QStringView text);

} // namespace selecta::core

Q_DECLARE_METATYPE(std::shared_ptr<const selecta::core::CollectionListing>)
