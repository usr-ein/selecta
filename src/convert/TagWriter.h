// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/StreamInfo.h"
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
    // ARTIST, ALBUMARTIST, COMPOSER, LYRICIST, REMIXER, CONDUCTOR, ORIGINALARTIST
    QString artistSeparator = QStringLiteral(", ");
    QString otherSeparator = QStringLiteral("; "); // every other field except URLs (one WXXX frame each)
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
// - M4A (files kept as they are, CONV-2): MP4 atoms and one `covr` image; the key goes to
//   `----:com.apple.iTunes:KEY`, the atom rekordbox reads (conversion §3).
// Label goes to TPUB, ISRC to TSRC and AUDIOSOURCEWEBPAGE (the source link, D30) to WOAS. GROUPING follows TagLib's
// mapping (ID3 GRP1, WORK is TIT1); which one rekordbox shows is an open question for the maintainer (conversion E-1).
//
// Threading: reentrant; call from a worker. Failure codes: OpenFailed (missing, or not writable), Unsupported (not
// MP3/AIFF/WAV/FLAC/M4A), TagError (TagLib can't parse the file or couldn't save it, e.g. on a full disk).
Result<void> writeTags(const QString &path, const MetadataModel &model, const TagWriteOptions &options = {});

// Whether writeTags() writes files of `container` (MP3, AIFF, WAV, FLAC, M4A); Ogg and Other containers it refuses
// with Unsupported. Pure.
bool canWriteTags(audio::Container container);

// What `model` looks like once written as ID3v2.3: TMOO/TSST/TDRL/TDTG/TPRO keys become the TXXX names TagLib reads
// back (MOOD, DISCSUBTITLE, RELEASEDATE, TAGGINGDATE, PRODUCEDNOTICE stay as keys but are stored as TXXX), multi-value
// fields are joined with the separators. Pure; the verifier compares a re-read file against this.
MetadataModel id3v23View(const MetadataModel &model, const TagWriteOptions &options = {});

// The text fields readTags() returns once writeTags() has written `model` into a file of `container`, predicted with
// TagLib in memory: the same frames, atoms or comments writeTags() builds, read back through TagLib's property map and
// readTags()'s aliases and totals. What the format can't hold is left out (MP4 track and disc numbers that aren't
// numbers; Vorbis field names with characters Vorbis forbids), and what TagLib converts is converted (ID3 numeric
// genres become names, MP4 BPM becomes a whole number). Values are not truncated, and pictures are not included.
// OutputVerifier compares a re-read file against this. Containers writeTags() refuses give the model unchanged.
//
// Threading: reentrant.
MetadataModel tagsAsWritten(const MetadataModel &model, audio::Container container,
                            const TagWriteOptions &options = {});

} // namespace selecta::convert
