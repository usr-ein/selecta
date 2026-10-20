// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace selecta::library {

// The result of a file operation: the final path on success, a reason on failure.
struct FileResult
{
    bool ok = false;
    QString path;  // the resulting file (published, filed or trashed location if known)
    QString error; // why it failed

    static FileResult success(QString path) { return {true, std::move(path), {}}; }
    static FileResult failure(QString error) { return {false, {}, std::move(error)}; }
};

// The only code that writes outside Selecta's caches (ADR-009, PRD principle 2):
// - in collection roots it can only create new files, never overwrite, move, rename or delete anything;
// - in the Inbox it can publish, rename and move files to the Trash;
// - every path is canonicalised and must stay inside its root (no "..", no symlink escapes).
// Moves within one volume use an atomic exclusive rename; across volumes, copy → flush → verify (SHA-256) → rename,
// then the source is removed. A crash at any step leaves either the old or the new file, never neither.
//
// Thread-safe: holds only immutable configuration. File I/O is blocking: call it from worker threads, never from the
// UI or engine threads.
class FileOps
{
public:
    // Moves a file to the Trash; returns false on failure. Injected so tests never touch the user's Trash.
    using TrashFunction = std::function<bool(const QString &path, QString *trashedPath)>;

    FileOps(const QString &inboxRoot, const QStringList &collectionRoots, TrashFunction trash = {});

    QString inboxRoot() const { return m_inboxRoot; }

    // Moves `sourceFile` (typically a verified temporary file) to `relativePath` inside the Inbox. If that name is
    // taken, " (2)", " (3)"… is added before the extension. Creates parent folders.
    FileResult publishIntoInbox(const QString &sourceFile, const QString &relativePath) const;

    // Files an Inbox file into a collection root at `relativePath`: copy (or rename on the same volume), verify, then
    // remove it from the Inbox. Refuses if the target exists. `collectionRoot` must be one of the configured roots.
    FileResult fileIntoCollection(const QString &inboxFile, const QString &collectionRoot,
                                  const QString &relativePath) const;

    // Files an Inbox folder (an album) into a collection root as `relativeFolder`, with everything in it. On one volume
    // the folder is renamed atomically. Across volumes every file is copied and verified into a hidden staging folder
    // next to the target, which is then renamed into place, so the target name only ever holds a complete album; the
    // Inbox folder goes only after that. Refuses if the target exists, or if the folder holds anything but plain files
    // and folders (symbolic links, for instance).
    FileResult fileFolderIntoCollection(const QString &inboxFolder, const QString &collectionRoot,
                                        const QString &relativeFolder) const;

    // Moves a file or folder inside the Inbox to the Trash. Anything outside the Inbox is refused.
    FileResult trashFromInbox(const QString &path) const;

    // Moves `sourceFolder` (a staged album, outside the Inbox) to `relativeFolder` inside the Inbox, with everything in
    // it: renamed on one volume, else copied and verified into a hidden folder next to the target that is then renamed
    // into place. A name already taken gets " (2)", " (3)"… Creates parent folders. The staged folder goes once the
    // album is in place.
    FileResult publishFolderIntoInbox(const QString &sourceFolder, const QString &relativeFolder) const;

    // Puts a file Selecta made (a conversion, in its staging folder) at `targetPath`, in a folder the user chose: a new
    // file only (CONV-3). A name already taken gets " (2)", " (3)"…; nothing is ever replaced. Moved on one volume,
    // else copied, verified and renamed into place; the staged file goes either way. The folder must exist.
    FileResult placeNewFile(const QString &stagedFile, const QString &targetPath) const;

    // Puts `newFile` (a verified copy, next to it) in the place of `inboxFile` in one atomic rename, so the name only
    // ever holds the old file or the new one (re-tagging, DL-4). Both must be inside the Inbox, on one volume.
    FileResult replaceInInbox(const QString &newFile, const QString &inboxFile) const;

    // Whether `path` (existing or not) resolves inside `root`.
    static bool isInside(const QString &path, const QString &root);

    // For tests: force the cross-volume copy path even within one volume.
    void setForceCopy(bool force) { m_forceCopy = force; }

private:
    FileResult moveExclusive(const QString &from, const QString &to) const;
    FileResult copyVerifyRename(const QString &from, const QString &to) const;
    // Checks that `collectionRoot` is configured and `relativePath` stays inside it; returns the absolute target.
    FileResult collectionTarget(const QString &collectionRoot, const QString &relativePath) const;
    FileResult copyFolderVerified(const QString &from, const QString &to) const;

    QString m_inboxRoot;
    QStringList m_collectionRoots;
    TrashFunction m_trash;
    bool m_forceCopy = false;
};

// Default Trash implementation (QFile::moveToTrash).
bool moveToSystemTrash(const QString &path, QString *trashedPath);

} // namespace selecta::library
