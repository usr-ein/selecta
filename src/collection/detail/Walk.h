// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"

#include <QSet>
#include <QStringView>

#include <vector>

// Internal: the folder walk of scan(), stat only. POSIX calls (opendir, fstatat, realpath) on the native paths, so
// every folder costs one listing and every entry one lstat.
namespace selecta::collection::detail {

// A folder the walk listed.
struct WalkedFolder
{
    QString path;           // as reached from its root (through symlinks inside the roots, if any)
    qsizetype root = 0;     // index into Walk::roots
    QStringList audioNames; // its audio files' names, sorted: siblings for the name parser
};

// An audio file the walk found.
struct WalkedFile
{
    FileSignature signature; // real path, size, modification time
    QString key;             // comparisonKey(path)
    qsizetype folder = 0;    // index into Walk::folders
};

struct Walk
{
    QStringList roots; // cleaned absolute roots, in the order given
    std::vector<WalkedFolder> folders;
    std::vector<WalkedFile> files; // in traversal order: roots in order, names sorted
    QList<QPair<QString, QString>> unreadable;
    // Comparison keys of the roots and folders whose contents are unknown (missing, unlistable, too deep), and of the
    // files that are present but weren't read (iCloud placeholders): known files there are never removed.
    QStringList blindFolders;
    QSet<QString> presentUnread;
    bool complete = true;   // every folder under the roots was listed: removals can be trusted
    bool cancelled = false; // stopped by the CancellationFlag
};

// Walks `roots` with the skip rules of scan(). Never opens a file; never follows a symlink out of the roots or into a
// folder it has seen. Threading: reentrant.
Walk walk(const QStringList &roots, const ScanOptions &options, const audio::CancellationFlag *cancel);

// Skip rules by name alone, for files (folders are skipped when hidden).
bool isHiddenName(QStringView name); // ".x" (dot files, AppleDouble "._x", .DS_Store)
bool isTemporaryName(
    QStringView name); // *.selecta-part, *.part, *.partial, *.tmp, *.temp, *.crdownload, *.download, ~$x, x~
bool isAudioName(QStringView name); // an audio extension (match::RemotePathParser::fileKind)

// Whether `key` is `folder` or lies under it (both comparison keys).
bool isUnder(QStringView key, QStringView folder);

} // namespace selecta::collection::detail
