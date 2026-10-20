// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "share/ShareIndex.h"
#include "share/detail/Attributes.h"
#include "share/detail/Descriptor.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>
#include <sys/stat.h>
#include <vector>

// Internal: build()'s folder walk, stat only. POSIX calls on the file system's own bytes: one listing per folder, one
// lstat per entry, nothing ever followed below a root.
namespace selecta::share::detail {

struct WalkedFile
{
    QByteArray nativeName;  // as the folder listing gave it
    QByteArray virtualName; // UTF-8 of LocalName::virtualName
    FileIdentity identity;
    Attributes attributes;
};

struct WalkedFolder
{
    QByteArray nativePath; // full local path: the root as given (QFile::encodeName), then names byte for byte
    QString virtualPath;   // <alias>\<folders…>
    QString segment;       // its own virtual name; the alias for a root folder
    qsizetype parent = -1; // index in Walk::folders; -1 for a root folder
    std::vector<WalkedFile> files; // sorted by virtual name bytes; virtual names are distinct
};

struct WalkRoot
{
    QString path;  // cleaned absolute path
    QString alias; // as peers will see it
};

struct Walk
{
    std::vector<WalkedFolder> folders; // in walk order: each folder before its subfolders, names sorted
    QList<QPair<QString, QString>> unreadable;
    qsizetype files = 0;
    bool cancelled = false;
};

// Longest virtual path kept: peers' requests over kMaxRequestPathBytes find nothing, so a longer one could be listed
// but never fetched.
inline constexpr qsizetype kMaxVirtualPathBytes = 4096;
// Strings the index holds at most (its pool offsets are 32-bit); the walk stops beyond, as at kMaxFiles.
inline constexpr qsizetype kMaxPoolBytes = qsizetype{1} << 30;

struct WalkOptions
{
    qsizetype maxFiles = kMaxFiles;
    int maxDepth = kMaxDepth;
    qsizetype maxPoolBytes = kMaxPoolBytes;
    qsizetype maxVirtualPathBytes = kMaxVirtualPathBytes;
    // Called after every successful lstat with the entry's local path; may change what the walk sees. Tests use it to
    // stand in for what they can't make (an iCloud placeholder) and to cancel at a chosen point. Null in build().
    std::function<void(const QByteArray &nativePath, struct stat &status)> afterStat;
};

// Walks `roots` with build()'s skip rules; audio files get their attributes from `attributes`. Threading: reentrant
// with one AttributeSource per call.
Walk walk(const QList<WalkRoot> &roots, const QStringList &excludedFolders, AttributeSource &attributes,
          const audio::CancellationFlag *cancel, const WalkOptions &options = {});

} // namespace selecta::share::detail
