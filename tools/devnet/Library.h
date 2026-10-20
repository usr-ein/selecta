// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Types.h"

#include <QHash>
#include <QList>
#include <QString>

namespace selecta::devnet {

// One peer of the local network and what it shares.
struct LibraryPeer
{
    QString username;
    slsk::SharedFileList shares;      // virtual paths `<username>\<folders…>`, as a sharing client sends them
    QHash<QByteArray, QString> files; // raw virtual path of each file → the local file it comes from
    quint64 totalBytes = 0;

    int fileCount() const { return static_cast<int>(files.size()); }
};

// A folder of folders as a set of peers: every subfolder of `root` is one peer, named after it, sharing everything
// below it. Hidden files and folders are skipped, and so is a subfolder whose name isn't a valid Soulseek username
// (reported in `skipped`). Audio files carry the attributes their headers give (AudioProbe.h); other files are
// shared bare, as real clients do. Folders and files are listed in name order.
QList<LibraryPeer> scanLibrary(const QString &root, QStringList *skipped = nullptr);

// One peer from one folder (the work scanLibrary() does per subfolder).
LibraryPeer scanPeer(const QString &username, const QString &folder);

} // namespace selecta::devnet
