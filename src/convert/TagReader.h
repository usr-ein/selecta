// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"

#include <QString>
#include <QStringView>

namespace selecta::convert {

struct TagReadOptions
{
    qint64 maxPictureBytes = qint64{20} * 1024 * 1024; // larger pictures are skipped (logged), not loaded
    int maxPictures = 8;                                // further pictures are skipped
    qint64 maxValueChars = 64 * 1024;                   // longer values (lyrics aside) are truncated
    qint64 maxLyricsChars = 256 * 1024;
};

// Reads every tag of an audio file into a MetadataModel (conversion §9.1, TagReader). Opens the file read-only (TagLib
// gets an explicit read-only stream, ADR-009), reads TagLib's property map and its PICTURE complex property, applies
// the alias table (canonicalTagKey) and marks every field Provenance::Embedded.
//
// The file is untrusted: TagLib ≥ 2.3 is required at build time, and pictures and values are bounded.
//
// Threading: reentrant; call from a worker (it reads the file). Failure codes: OpenFailed, Unsupported (not a format
// TagLib knows), TagError.
Result<MetadataModel> readTags(const QString &path, const TagReadOptions &options = {});

// The alias table of conversion §9.1, for keys read from files: upper-cases and maps synonyms to the canonical key
// ("ALBUM ARTIST", "ALBUM_ARTIST" → ALBUMARTIST; "KEY" → INITIALKEY; "ORGANIZATION", "PUBLISHER" → LABEL;
// "DESCRIPTION" → COMMENT; "UNSYNCEDLYRICS" → LYRICS; "ORIGINALYEAR" → ORIGINALDATE; "CATALOGID", "DISCOGS_CATALOG" →
// CATALOGNUMBER). Totals (TRACKTOTAL, TOTALTRACKS, DISCTOTAL, TOTALDISCS) are folded into "n/N" by readTags(), not
// here.
QString canonicalTagKey(QStringView rawKey);

} // namespace selecta::convert
