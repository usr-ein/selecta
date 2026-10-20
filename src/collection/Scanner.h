// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "convert/TagReader.h"
#include "match/Types.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

// The collection scanner (docs/contracts/collection.md): reads the user's music folders, read-only, into what
// match::MatchIndex::build needs. No database, no settings, no network.
namespace selecta::collection {

// What tells a changed file from an unchanged one.
struct FileSignature
{
    QString path;          // absolute, as found on disk (macOS may give NFD names)
    qint64 size = 0;       // bytes
    qint64 modifiedMs = 0; // last modification, milliseconds since the epoch

    friend bool operator==(const FileSignature &, const FileSignature &) = default;
};

// One audio file: tags, stream facts and identity, ready to become a match::LibraryTrack.
struct ScannedFile
{
    FileSignature file;
    QString artist; // ARTIST; several values joined with " & " (the matcher splits artist sets on it)
    QString title;  // with its version text, as tagged ("Glue (Original Mix)")
    QString album;
    QString albumArtist;
    int trackNumber = 0;   // 0 = unknown ("3/12" gives 3)
    int discNumber = 0;    // 0 = unknown
    int year = 0;          // from DATE (else ORIGINALDATE); 0 = unknown
    qint64 durationMs = 0; // from the stream headers; 0 = unknown
    match::AudioQuality quality;
    QString isrc;
    bool namesFromFileName = false; // the file has no title tag: missing names were parsed from the file and folders
};

struct ScanResult
{
    QList<ScannedFile> added;   // new paths, sorted by path
    QList<ScannedFile> changed; // known paths whose signature differs, sorted by path
    QStringList removed;        // known paths (as given) under a scanned root that no longer exist, sorted
    // (path, reason): files that couldn't be read (corrupt, not audio after all, an iCloud placeholder), folders that
    // couldn't be listed, roots that aren't there. Known files under an unreadable folder or root are not removed.
    QList<QPair<QString, QString>> unreadable;
    bool cancelled = false; // see scan()
};

// Tags read during a scan: no pictures, short values, no lyrics.
inline constexpr convert::TagReadOptions kScanTagOptions{
    .maxPictureBytes = 0, .maxPictures = 0, .maxValueChars = 4096, .maxLyricsChars = 0};

struct ScanOptions
{
    int readers = 4; // files read at once: 4–8 suit an SSD, 1–2 a spinning disk; clamped to 1…16
    convert::TagReadOptions tags = kScanTagOptions;
    // Absolute folders under the roots that are never scanned (Selecta's own Inbox and caches). Known files under them
    // are reported as removed: they aren't part of the collection.
    QStringList excludedFolders;
    // Folder roles (PRD LIB-1, FolderRoles): of a tracks folder only the files lying in it are scanned, never its
    // subfolders; of an albums folder only its subfolders, never the files lying in it. Absolute folders, applied
    // wherever the walk meets them, as a root or below one; a folder in both lists is scanned whole (loose tracks and
    // album folders side by side). Known files these rules leave out are reported as removed.
    QStringList tracksFolders;
    QStringList albumsFolders;
    qint64 maxFiles = 1'000'000; // more audio files than this: the rest aren't scanned, and nothing is removed
    int maxDepth = 64;           // folders nested deeper aren't scanned (reported as unreadable)
};

// Scans `roots` and compares what it finds with `known` (the signatures of the previous scan, keyed by path).
//
// Only files whose signature differs from `known` (or that aren't in it) are read: their tags through
// convert::readTags (TagLib on a read-only stream, options.tags bounds) and their stream facts through
// audio::Decoder::open() on the header alone. Paths are compared normalised to NFC (macOS may report NFD), results
// carry the real on-disk path. Skipped: hidden files and folders, AppleDouble files, temporary files (*.selecta-part,
// *.part, *.tmp, …), non-audio extensions, excluded folders, what the folder roles leave out (subfolders of a tracks
// folder, files lying in an albums folder), symlinks that point outside the roots; folders and files are visited once
// by (device, inode), so loops are never followed.
//
// Blocking. Reads on up to options.readers threads of its own; `progress(done, total)` is called on the calling thread
// as files are read (total: the files to read). `cancel` is polled between files: a cancelled scan returns at once
// with `cancelled` set, the files read so far in added/changed, and nothing in removed unless the walk had finished.
//
// Threading: reentrant; call from a worker, never from the UI thread. Failure modes: none thrown; whatever can't be
// read goes to ScanResult::unreadable. Never writes a byte under the roots.
ScanResult scan(const QStringList &roots, const QHash<QString, FileSignature> &known, const ScanOptions &options = {},
                const audio::CancellationFlag *cancel = nullptr,
                const std::function<void(int done, int total)> &progress = {});

// The key paths are compared by: absolute, cleaned, NFC. Pure; any thread.
QString comparisonKey(const QString &path);

} // namespace selecta::collection
