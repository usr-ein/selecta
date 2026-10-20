// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "core/InboxService.h"
#include "library/FileOps.h"
#include "library/InboxStore.h"
#include "meta/Release.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace selecta::core {

// One downloaded file of an album.
struct AlbumArrivalFile
{
    qint64 downloadId = 0;
    QString localPath;  // the original, as downloaded
    QString remoteName; // its name on the peer
};

// An album whose files are all in (PRD DL-3): what the worker needs to make it an Inbox album.
struct AlbumArrivalJob
{
    qint64 albumId = 0;
    QList<AlbumArrivalFile> files;        // audio and images
    int missing = 0;                      // files that couldn't be downloaded
    std::optional<meta::Release> release; // a track on the album
    QList<meta::Release> tracks;          // the album's tracks, when it could be looked up
    QByteArray cover;                     // the album's cover, fetched; empty: the folder's image, else the files'
    QString folderName;                   // the folder's name on the peer
    QString peer;
    LibraryConfig library;
    QString stagingDir;
    QString encodedBy;
    library::FileOps::TrashFunction trash;
};

struct AlbumArrivalResult
{
    struct Track
    {
        qint64 downloadId = 0;
        bool ok = false;
        QString error;
        QString artist;
        QString title;
        qint64 durationMs = 0;
        QString sourceQuality; // as downloaded: "FLAC 16/44.1"
        qint64 sizeBytes = 0;
        quint64 releaseTrack = 0; // 1-based position in AlbumArrivalJob::tracks it was matched to, 0: none
    };

    bool ok = false;
    bool cancelled = false;
    bool complete = false; // every audio file of the folder arrived and was converted
    QString error;
    QString warning;
    library::InboxItem item; // the album, Kind::Album, relative to the Inbox
    QList<Track> tracks;     // the audio files, in track order
};

// Makes the files of an album download one Inbox album (PRD DL-3, D29): each audio file is converted to the Inbox
// format (or kept when that can't improve it), tagged from its track on the album when the album's track list is known
// (matched by disc and track number, then by name and length), else from its own tags with the album's fields, given
// the album cover (fetched, else the folder's image, else its own) and named by the album track template; the cover
// is saved as cover.jpg too. The album folder, named by the album folder template, is assembled in staging and
// published into Inbox/Albums in one move, the folder and every file in it quarantined (NFR-4). Originals then go:
// converted lossless files to the Trash (D11), the rest deleted.
//
// Threading: blocking file work; call it from a worker. Failure modes: a file that can't be read or converted is left
// out and reported in its Track; when no audio file makes it, the album fails and nothing is published.
AlbumArrivalResult arriveAlbum(const AlbumArrivalJob &job, const audio::CancellationFlag *cancel);

} // namespace selecta::core
