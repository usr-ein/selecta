// SPDX-License-Identifier: GPL-3.0-or-later
#include "Library.h"

#include "AudioProbe.h"
#include "slsk/PeerMessages.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>
#include <map>

using namespace Qt::StringLiterals;

namespace selecta::devnet {
namespace {

// `<username>\<relative folder, backslashes>`; the peer's folder itself is just `<username>`.
QString virtualFolder(const QString &username, const QString &relative)
{
    if (relative.isEmpty() || relative == u"."_s) {
        return username;
    }
    return username + u'\\' + QString(relative).replace(u'/', u'\\');
}

bool hidden(const QString &relativePath)
{
    const QStringList parts = relativePath.split(u'/', Qt::SkipEmptyParts);
    return std::ranges::any_of(parts, [](const QString &part) { return part.startsWith(u'.'); });
}

} // namespace

LibraryPeer scanPeer(const QString &username, const QString &folder)
{
    LibraryPeer peer;
    peer.username = username;
    const QDir base(folder);
    std::map<QString, QList<slsk::FileEntry>> byFolder; // relative folder → its files
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        if (hidden(base.relativeFilePath(info.filePath()))) {
            continue;
        }
        const QString relativeFolder = base.relativeFilePath(info.path());
        slsk::FileEntry entry;
        entry.name = slsk::RemotePath::fromText(info.fileName());
        entry.size = static_cast<quint64>(info.size());
        entry.extension = info.suffix().toLower();
        entry.attributes = probeAudio(info.filePath());
        const QString virtualPath = virtualFolder(username, relativeFolder) + u'\\' + info.fileName();
        peer.files.insert(slsk::RemotePath::fromText(virtualPath).raw, info.filePath());
        peer.totalBytes += entry.size;
        byFolder[relativeFolder].append(std::move(entry));
    }
    for (auto &[relative, entries] : byFolder) {
        std::ranges::sort(entries, [](const slsk::FileEntry &a, const slsk::FileEntry &b) {
            return QString::compare(a.name.display(), b.name.display(), Qt::CaseInsensitive) < 0;
        });
        peer.shares.directories.append(
            slsk::SharedDirectory{slsk::RemotePath::fromText(virtualFolder(username, relative)), entries});
    }
    return peer;
}

QList<LibraryPeer> scanLibrary(const QString &root, QStringList *skipped)
{
    QList<LibraryPeer> peers;
    const QFileInfoList folders = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &folder : folders) {
        const QString name = folder.fileName();
        if (!slsk::isAcceptablePeerUsername(name)) {
            if (skipped != nullptr) {
                skipped->append(name);
            }
            continue;
        }
        peers.append(scanPeer(name, folder.filePath()));
    }
    return peers;
}

} // namespace selecta::devnet
