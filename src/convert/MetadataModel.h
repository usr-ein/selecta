// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"

#include <QByteArray>
#include <QList>
#include <QLoggingCategory>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

Q_DECLARE_LOGGING_CATEGORY(lcConvert)

namespace selecta::convert {

using audio::Error;
using audio::ErrorCode;
template <typename T>
using Result = audio::Result<T>;

// Where a field's value came from. Drives the "shown for confirmation" UI of CONV-5 and META-4.
enum class Provenance {
    Unknown,
    Embedded, // read from the file's own tags
    Release,  // the matched release (Spotify, MusicBrainz, Deezer…)
    Folder,   // the folder: a cover image file next to the audio
    FileName, // parsed from the file or folder name
    User,     // typed or confirmed by the user
    Selecta,  // set by Selecta itself (encoder, TENC)
};

// Canonical field names: TagLib property keys, upper case (conversion §3). Any other upper-case key is allowed and is
// written as a TXXX frame / Vorbis comment of that name.
namespace keys {
inline constexpr QStringView Title = u"TITLE";
inline constexpr QStringView Artist = u"ARTIST";
inline constexpr QStringView Album = u"ALBUM";
inline constexpr QStringView AlbumArtist = u"ALBUMARTIST";
inline constexpr QStringView TrackNumber = u"TRACKNUMBER"; // "n" or "n/N"
inline constexpr QStringView DiscNumber = u"DISCNUMBER";   // "n" or "n/N"
inline constexpr QStringView Date = u"DATE";               // "YYYY" or "YYYY-MM-DD"
inline constexpr QStringView OriginalDate = u"ORIGINALDATE";
inline constexpr QStringView ReleaseDate = u"RELEASEDATE";
inline constexpr QStringView Genre = u"GENRE";
inline constexpr QStringView Composer = u"COMPOSER";
inline constexpr QStringView Lyricist = u"LYRICIST";
inline constexpr QStringView Remixer = u"REMIXER";
inline constexpr QStringView Conductor = u"CONDUCTOR";
inline constexpr QStringView Bpm = u"BPM";               // copied verbatim, decimals included
inline constexpr QStringView InitialKey = u"INITIALKEY"; // copied verbatim (Camelot, Open Key, "Dbm"…)
inline constexpr QStringView Label = u"LABEL";           // ID3 TPUB (D30)
inline constexpr QStringView Isrc = u"ISRC";             // ID3 TSRC (D30)
inline constexpr QStringView CatalogNumber = u"CATALOGNUMBER";
inline constexpr QStringView Comment = u"COMMENT";
inline constexpr QStringView Lyrics = u"LYRICS";
inline constexpr QStringView Compilation = u"COMPILATION";
inline constexpr QStringView Grouping = u"GROUPING";
inline constexpr QStringView Work = u"WORK";
inline constexpr QStringView Subtitle = u"SUBTITLE";
inline constexpr QStringView Mood = u"MOOD";
inline constexpr QStringView DiscSubtitle = u"DISCSUBTITLE";
inline constexpr QStringView EncodedBy = u"ENCODEDBY";   // ID3 TENC
inline constexpr QStringView Encoding = u"ENCODING";     // ID3 TSSE: encoder settings
inline constexpr QStringView Copyright = u"COPYRIGHT";
inline constexpr QStringView TaggingDate = u"TAGGINGDATE";     // ID3v2.4 TDTG
inline constexpr QStringView ProducedNotice = u"PRODUCEDNOTICE"; // ID3v2.4 TPRO
inline constexpr QStringView SourceUrl = u"URL";        // the source link (D30): ID3 WOAS/WXXX, Vorbis URL
} // namespace keys

// An embedded picture. Pictures are bounded when read (TagReadOptions).
struct Picture
{
    // ID3v2 APIC / FLAC PICTURE type numbers.
    static constexpr int kTypeOther = 0;
    static constexpr int kTypeFrontCover = 3;
    static constexpr int kTypeBackCover = 4;

    QByteArray data;
    QString mimeType; // "image/jpeg", "image/png"; empty when the container doesn't say
    int type = kTypeFrontCover;
    QString description;
    Provenance provenance = Provenance::Unknown;

    bool isFrontCover() const { return type == kTypeFrontCover; }
    friend bool operator==(const Picture &, const Picture &) = default;
};

// Selecta's metadata model (conversion §9.1): canonical fields with several values each, pictures, and per-field
// provenance. A plain value type: copyable, comparable, no thread affinity.
//
// Keys are stored upper case. Values keep their exact text: nothing is normalised (keys, BPM and labels are copied
// verbatim, conversion §3). A field with no values doesn't exist.
class MetadataModel
{
public:
    struct Field
    {
        QStringList values;
        Provenance provenance = Provenance::Unknown;
        friend bool operator==(const Field &, const Field &) = default;
    };

    bool isEmpty() const { return m_fields.isEmpty() && m_pictures.isEmpty(); }
    // Sorted.
    QStringList keys() const { return m_fields.keys(); }
    bool contains(QStringView key) const;
    QStringList values(QStringView key) const;
    // The first value, or an empty string.
    QString value(QStringView key) const;
    Provenance provenance(QStringView key) const;
    const QMap<QString, Field> &fields() const { return m_fields; }

    // Replaces a field. Empty strings are dropped from `values`; nothing left removes the field.
    void set(QStringView key, const QStringList &values, Provenance provenance);
    void set(QStringView key, const QString &value, Provenance provenance);
    void remove(QStringView key);

    const QList<Picture> &pictures() const { return m_pictures; }
    // The first front cover, else nullopt.
    std::optional<Picture> frontCover() const;
    void setPictures(const QList<Picture> &pictures) { m_pictures = pictures; }
    // Replaces every picture with this one front cover (type forced to front cover): what Selecta writes.
    void setFrontCover(const Picture &picture);
    void clearPictures() { m_pictures.clear(); }

    // CONV-5: adds the fields `other` has and this model lacks (with their provenance), and its front cover when this
    // model has none.
    void supplementFrom(const MetadataModel &other);
    // Replaces fields with those `other` has (a matched release overriding the file's own tags); pictures too when
    // `other` has a front cover.
    void overrideFrom(const MetadataModel &other);

    friend bool operator==(const MetadataModel &, const MetadataModel &) = default;

private:
    QMap<QString, Field> m_fields;
    QList<Picture> m_pictures;
};

QString provenanceName(Provenance provenance);

} // namespace selecta::convert

Q_DECLARE_METATYPE(selecta::convert::MetadataModel)
