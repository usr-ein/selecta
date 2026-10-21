// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "share/ShareIndex.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <sys/stat.h>

// Helpers for the share tests: a temporary folder tree (tests never touch anything outside it), synthetic audio, and
// what an index looks like through slsk's own decoders.
namespace selecta::share::test {

class Tree
{
public:
    Tree();

    bool isValid() const { return m_dir.isValid(); }
    // The temporary folder, as QTemporaryDir spells it (on macOS under /var, a symlink to /private/var).
    QString root() const { return m_dir.path(); }
    QString path(const QString &relative) const { return m_dir.filePath(relative); }

    // Writes `bytes` at `relative`, creating folders. Returns the absolute path; empty (and a failed expectation) on
    // failure.
    QString file(const QString &relative, const QByteArray &bytes = QByteArrayLiteral("data")) const;
    // Writes `bytes` at a path given as raw bytes under the root (names Qt would re-encode, such as NFD).
    QByteArray nativeFile(const QByteArray &relative, const QByteArray &bytes = QByteArrayLiteral("data")) const;
    QString folder(const QString &relative) const;
    // A symlink at `relative` pointing at `target` (absolute or relative, as given).
    bool symlink(const QString &target, const QString &relative) const;

private:
    QTemporaryDir m_dir;
};

// chflags(2) on the entry itself. The hidden flag is macOS's: elsewhere this returns false.
bool setHidden(const QString &path);
bool setMode(const QString &path, int mode);
// A stat's modification time in milliseconds, as the collection records it (st_mtimespec on macOS, st_mtim elsewhere).
qint64 modifiedMs(const struct stat &status);

// Real audio, generated (tests/audio/support), cached per kind.
QByteArray mp3Cbr320();
QByteArray mp3Vbr();
QByteArray flac(int bits);

// What the collection would report for `path` (its current size and modification time) with these facts.
collection::ScannedFile scanned(const QString &path, const match::AudioQuality &quality, qint64 durationMs);

// build() over one tree's root, aliased "Music": an albums folder unless told otherwise.
BuildResult buildTree(const Tree &tree, const QHash<QString, collection::ScannedFile> &knownAudio = {},
                      const BuildOptions &options = {}, Layout layout = Layout::Albums);

// The share list through slsk::peer::SharedFileListResponse::decode().
slsk::SharedFileList decodedList(const ShareIndex &index);
// Every shared file's full virtual path, from the decoded share list, in its order.
QStringList listedFiles(const ShareIndex &index);
// Every folder of the decoded share list, in its order.
QStringList listedFolders(const ShareIndex &index);
// The full virtual paths of search results.
QStringList displayPaths(const QList<slsk::FileEntry> &entries);
// The local paths of unreadable entries.
QStringList unreadablePaths(const BuildResult &result);
// The decoded share list's entry for a full virtual path, or nullopt.
std::optional<slsk::FileEntry> listedEntry(const ShareIndex &index, const QString &virtualPath);

} // namespace selecta::share::test
