// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"

#include <QString>

namespace selecta::convert {

enum class Id3Version {
    V23, // default (D-1): UTF-16 text, readable everywhere
    V24, // option (CONV-4): UTF-8 text, native multi-values and dates
};

struct TagWriteOptions
{
    Id3Version id3Version = Id3Version::V23;
    // ID3v2.3 has no multi-value text frames: values are joined (D-6). "/" is avoided because it occurs in names.
    QString artistSeparator = QStringLiteral(", "); // ARTIST, ALBUMARTIST, COMPOSER, LYRICIST, REMIXER, CONDUCTOR
    QString otherSeparator = QStringLiteral("; ");
};

// Writes `model` as the complete tag set of the file at `path`: every existing tag is replaced, so nothing stale
// survives (conversion §9.1, TagWriter). Only for files Selecta just wrote (encoder outputs, copies being re-tagged);
// TagLib rewrites the file in place.
//
// Per format, detected from the content:
// - MP3: ID3v2.3 (or 2.4) only: ID3v1 and APE are stripped; UTF-16 text; one APIC front cover with an empty Latin-1
//   description; TMOO, TSST, TDRL, TDTG and TPRO remapped to TXXX for v2.3; multiple values joined (id3v23View).
// - AIFF: the same ID3v2 tag in an `ID3 ` chunk.
// - WAV: RIFF INFO (the only tags rekordbox reads) plus the same ID3v2 tag in an `ID3 ` chunk.
// - FLAC: Vorbis comments (multiple values native) and one PICTURE block; ID3 tags some tools prepend are stripped.
// URL (the source link, D30) becomes WOAS when it is the only value, WXXX entries otherwise.
//
// Threading: reentrant; call from a worker. Failure codes: OpenFailed, Unsupported (not MP3/AIFF/WAV/FLAC), TagError
// (TagLib refused to save), WriteFailed.
Result<void> writeTags(const QString &path, const MetadataModel &model, const TagWriteOptions &options = {});

// What `model` looks like once written as ID3v2.3: TMOO/TSST/TDRL/TDTG/TPRO keys become the TXXX names TagLib reads
// back (MOOD, DISCSUBTITLE, RELEASEDATE, TAGGINGDATE, PRODUCEDNOTICE stay as keys but are stored as TXXX), multi-value
// fields are joined with the separators. Pure; the verifier compares a re-read file against this.
MetadataModel id3v23View(const MetadataModel &model, const TagWriteOptions &options = {});

} // namespace selecta::convert
