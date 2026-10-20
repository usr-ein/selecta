// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"
#include "match/MatchIndex.h"

#include <QHash>
#include <QList>
#include <QString>

namespace selecta::collection {

inline constexpr int kAlbumMinTracks = 3;               // tracks sharing an album tag that make a folder an album
inline constexpr double kVariousArtistsTopShare = 0.25; // the most common artist below this share: various artists
inline constexpr double kVariousArtistsDistinct = 0.75; // or more than this share of the tracks have distinct artists

// The album folders among scanned files (note §8 step 1, PRD LIB-1, DL-11 "already on an album you own").
//
// A folder is an album when at least kAlbumMinTracks of its tracks share an album tag, or when its name says so
// ("Artist - Album", "Artist - Album (2019)", "[CAT001] Artist - Album", "Album (2019)") and its tracks are numbered
// or tagged with that album. Disc folders ("CD1", "CD 2", "Disc 2", "Disk-3") merge into their parent folder.
// Various artists: an album artist tag saying so ("Various Artists", "VA"), or track artists where the most common
// covers less than 25 % of the tracks or more than 75 % are distinct (Lidarr's rule).
//
// The album takes the most common album tag (else the name of the folder), album artist (else the most common track
// artist, "Various Artists" for a compilation) and year; source is Collection (core sets Inbox albums). Ids are stable
// across scans: a hash of the album folder's NFC path, unique within the result. `albumIdOfPath`, when given, receives
// the album id of every file that belongs to an album (keyed by the file's path as given); other files are left out.
// Albums are sorted by folder.
//
// Threading: pure; any thread. Failure modes: none.
QList<match::LibraryAlbum> groupAlbums(const QList<ScannedFile> &files, QHash<QString, quint64> *albumIdOfPath);

} // namespace selecta::collection
