// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Normalizer.h"
#include "match/Types.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace selecta::match {

class ArtistDictionary;

// The rule that produced a parsed field (note §4.1). Recorded for confidence and for the inspector's explanation.
enum class ParseRule : quint8 {
    None,
    // Cleaning
    MojibakeRepaired, // UTF-8 read as Latin-1 repaired before parsing
    PathTruncated,    // the path was longer than kMaxPathLength: only its tail was parsed
    WebsiteStripped,  // "www.site.com - " prefix or suffix removed
    WebJunkStripped,  // "(Official Video)", "[HQ]", "(Lyrics)", "[Free Download]"…
    QualityTag,       // "[FLAC 24-96]", "(320)", "[V0]", "WEB": moved to qualityHint
    CatalogueNumber,  // "[DC123]", "(ABC 123)": moved to catalogue
    YearTag,          // "(2019)", "[2019]" in a name
    // Numbering
    TrackPrefix,       // "03 - Title", "03. Title", "03 Title" (leading zero or confirmed by siblings)
    DiscTrackPrefix,   // "1-05 Title", "2.03 Title"
    HundredsDiscTrack, // "101 …", "201 …": disc × 100 + track, confirmed by siblings
    VinylPosition,     // "A1 Title", "B2. Title"
    MidTrackNumber,    // "Artist - Album - 01 Title" (Bandcamp)
    NumberOnlyName,    // "01.flac": a track number and no title
    GenericRipName,    // "Track 01", "AudioTrack 01": no title
    // Filename layout
    ArtistTitle,         // "Artist - Title"
    ArtistAlbumTitle,    // "Artist - Album - Title" (Bandcamp: the middle segment is the folder's album)
    TitleVersionSegment, // "Artist - Title - Extended Mix": the last segment is version text
    SceneSplit,          // "artist-title" split on bare hyphens in a scene-style folder
    TitleOnly,           // a title with no artist segment; the artist comes from a folder
    WholeName,           // fallback: the whole name is the title (no segment split)
    // Artist/title order (note §4.1 step 6, §17.5 A)
    OrderSiblingConstant, // the segment constant across ≥ 3 siblings is the artist
    OrderFolderArtist,    // the segment equal to the folder's artist is the artist
    OrderKnownArtist,     // the segment found in the collection's artist dictionary is the artist
    OrderReleaseArtist,   // the segment found among the current release candidates' artists is the artist
    OrderTitleFeatures,   // version brackets, feat. or a track number mark the other segment as the title
    OrderPrior,           // default "Artist - Title" order
    // Folders
    ArtistFolder,         // artist from the folder above the album folder
    ArtistAlbumFolder,    // "Artist - Album (Year)" folder split
    AlbumArtistFolder,    // "Album - Artist" folder split, reversed by artist evidence
    AlbumFolder,          // album from the album folder
    SceneFolder,          // "Artist-Album-WEB-2019-GRP"
    DiscFolder,           // "CD2", "Disc 2": disc number, album from the folder above
    VariousArtistsFolder, // "VA - …", "Various Artists", iTunes "Compilations"
    VariousArtistsFiles,  // artists vary across the folder's files (note §8)
    SiblingArtist,        // the artist of every sibling file is the same (album artist)
};

// The kind of a remote file, from its extension (note §4.1 step 1, §14).
enum class FileKind : quint8 {
    Audio,     // mp3 flac wav aif aiff m4a alac aac ogg opus wma ape wv
    Companion, // cover images, cue, log, m3u, nfo, txt, pdf, accurip…
    Other,     // anything else, including executables: never a track
};

// A parsed field: its value, a confidence in [0, 1] and the rule that set it. An unset field has rule None.
template <typename T>
struct ParsedField
{
    T value{};
    float confidence = 0.0F;
    ParseRule rule = ParseRule::None;

    bool isSet() const { return rule != ParseRule::None; }
};

// The zones of a path for reference-guided scoring (note §17.5 B). Display text after cleaning.
struct PathZones
{
    QString body;         // F: the filename without extension, numbering, quality, catalogue, year and junk tags
    QStringList segments; // F split on spaced dashes (scene names: on bare hyphens)
    QStringList brackets; // B: the filename's bracket groups (version, feat., extras), without brackets
    QString parent;       // D1: the album folder (disc folders skipped), cleaned; empty when generic
    QStringList higher;   // D+: the non-generic folders above D1, nearest first (at most 4)
};

// A remote path turned into fields (note §4.1 step 9). Display values are cleaned text as written (not keys); keys
// holds Normalizer::trackKeys() of artist, title and album.
struct ParsedRemote
{
    QString path;      // the input path (its tail when truncated)
    QString fileName;  // the last component
    QString extension; // lower case, without the dot; empty when there is none
    FileKind kind = FileKind::Other;

    ParsedField<QString> artist;      // primary artist(s) as written, feat. removed ("Above & Beyond")
    QStringList featured;             // featured artists as written
    ParsedField<QString> title;       // the title with its version text ("Glue (Original Mix)", "Song - Extended Mix")
    VersionInfo version;              // the classified version of the title
    ParsedField<QString> album;       // from the folder layout
    ParsedField<QString> albumArtist; // the folder's artist, when there is one
    ParsedField<int> year;
    ParsedField<int> disc;
    ParsedField<int> track;
    QString vinylPosition; // "B2"
    QString catalogue;     // "DC123"
    QString qualityHint;   // "FLAC 24-96", "320", "V0"
    QString sourceHint;    // "WEB", "CD", "VINYL"

    bool variousArtists = false; // a compilation folder (note §8)
    bool orderAmbiguous = false; // artist/title order evidence within a small margin: cap the tier at likely (§17.5)
    bool wholeNameOnly = false;  // no segment split was possible (rule WholeName): the matcher's × 0.9 penalty
    bool unparseable = false;    // no usable title ("Track 01", "01.flac"): history matching only (T19)
    bool truncated = false;      // the path was cut to kMaxPathLength

    PathZones zones;
    TrackKeys keys;         // Normalizer::trackKeys(artist, title, album)
    QList<ParseRule> rules; // every rule that fired, in order
};

// Optional evidence for the artist/title order. Pointers are not owned and must stay valid during the call.
struct ParseContext
{
    const ArtistDictionary *knownArtists = nullptr;   // the collection's artists (MatchIndex::artists())
    const ArtistDictionary *releaseArtists = nullptr; // the current search's release candidates (SRCH-17)
};

// Remote path → ParsedRemote (note §4.1, §17.5 A).
//
// The path is the virtual path exactly as received, backslash-separated ("/" when it has no backslash). Siblings are
// the file names (or paths: only their last component is used) of the other files in the same remote folder, from the
// same response; they validate one filename pattern across the folder (beets' fromfilename), confirm track number
// sequences and reveal the constant artist segment. Hand-written scanners, no regular expressions.
//
// Threading: pure static functions; call them from any thread concurrently.
// Failure modes: none; untrusted input is bounded. A path longer than kMaxPathLength is cut to its tail (truncated);
// only the last kMaxFolders folders are looked at; at most kMaxSiblings siblings are used. A name that can't be parsed
// gives unparseable = true and empty title/artist rather than garbage.
class RemotePathParser final
{
public:
    static constexpr qsizetype kMaxPathLength = 4096;
    static constexpr qsizetype kMaxFolders = 6;
    static constexpr qsizetype kMaxSiblings = 2000;

    // Parses one path. `siblingNames` may include the file itself.
    static ParsedRemote parse(QStringView path, const QStringList &siblingNames = {}, ParseContext context = {});

    // Parses every file of one remote folder (names or full paths) together: the folder is analysed once, so this is
    // the efficient call for a search response. `folder` is the folder's path; results are in the order of `files`.
    static QList<ParsedRemote> parseFolder(QStringView folder, const QStringList &files, ParseContext context = {});

    static FileKind fileKind(QStringView extension);
};

} // namespace selecta::match
