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
// - in collection roots it can only create new files, never overwrite, move, rename or delete anything, with one
//   exception the user asks for: an upgrade filed takes a collection file's place, and that file goes to the Trash
//   (replaceInCollection(), P-010, PRD D41);
// - in the Inbox it can publish, rename and move files to the Trash;
// - every path is canonicalised and must stay inside its root (no "..", no symlink escapes).
// Moves within one volume use an atomic exclusive rename; across volumes, copy → flush → verify (SHA-256) → rename,
// then the source is removed. A crash at any step leaves either the old or the new file, never neither.
//
// What goes into the Inbox came from a peer (D20): it carries the macOS quarantine attribute before it appears there
// (NFR-4, library/Quarantine.h), and keeps the one it already has. Moves and copies keep a file's attribute, filing
// into the collection included; nothing ever adds one to a collection file. A volume that can't take the attribute
// doesn't stop a file: the log says so.
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

    // Moves `sourceFile` (typically a verified temporary file) to `relativePath` inside the Inbox, quarantined. If that
    // name is taken, " (2)", " (3)"… is added before the extension. Creates parent folders.
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

    // Files an Inbox file (an upgrade, P-010) in the place of `collectionFile` (PRD D41): at its exact path, or, when
    // that file is another lossy type (AAC, Ogg, Opus, WMA) and the upgrade an MP3, under its name with .mp3
    // (upgradeTarget()). A copy is made and verified (SHA-256) under a hidden name beside it, the old file goes to the
    // Trash, then the copy is renamed into place (exclusively), and the Inbox file goes. Should that rename fail, the
    // old file is put back from the Trash. The path holds the old file until the new one takes its place; it is empty
    // only for the moment between the two steps, and a crash there leaves the old file in the Trash and the new one in
    // the Inbox: saving it again fills the path. A path no file holds any more simply gets the new one. Refused: an
    // Inbox file that isn't one, a path outside the collection folders or in a folder that isn't there, a symbolic
    // link, a type the name can't keep, and an .mp3 name already taken by another file.
    FileResult replaceInCollection(const QString &inboxFile, const QString &collectionFile) const;
    // What replaceInCollection() checks before it touches anything: the path the upgrade would take (real, as filed
    // paths are reported), or why it won't. Reads the disk (stat).
    FileResult checkReplace(const QString &inboxFile, const QString &collectionFile) const;
    // The path an upgrade takes in place of `collectionFile`: the same path when both are of one type (extension,
    // without case); the same name with .mp3 when the upgrade is an MP3 and the file another lossy type (.m4a, .mp4,
    // .aac, .ogg, .oga, .opus, .wma); empty when neither. Pure.
    static QString upgradeTarget(const QString &upgradeFile, const QString &collectionFile);

    // Moves `sourceFolder` (a staged album, outside the Inbox) to `relativeFolder` inside the Inbox, with everything in
    // it, all quarantined: renamed on one volume, else copied and verified into a hidden folder next to the target that
    // is then renamed into place. A name already taken gets " (2)", " (3)"… Creates parent folders. The staged folder
    // goes once the album is in place.
    FileResult publishFolderIntoInbox(const QString &sourceFolder, const QString &relativeFolder) const;

    // Puts a file Selecta made (a conversion, in its staging folder) at `targetPath`, in a folder the user chose: a new
    // file only (CONV-3). A name already taken gets " (2)", " (3)"…; nothing is ever replaced. Moved on one volume,
    // else copied, verified and renamed into place; the staged file goes either way. The folder must exist.
    FileResult placeNewFile(const QString &stagedFile, const QString &targetPath) const;

    // Puts `newFile` (a verified copy, next to it) in the place of `inboxFile` in one atomic rename, so the name only
    // ever holds the old file or the new one (re-tagging, DL-4). Both must be inside the Inbox, on one volume. The new
    // file takes the old one's quarantine attribute.
    FileResult replaceInInbox(const QString &newFile, const QString &inboxFile) const;

    // Whether `path` (existing or not) resolves inside `root`.
    static bool isInside(const QString &path, const QString &root);

    // For tests: force the cross-volume copy path even within one volume.
    void setForceCopy(bool force) { m_forceCopy = force; }

private:
    FileResult moveExclusive(const QString &from, const QString &to) const;
    FileResult copyVerifyRename(const QString &from, const QString &to) const;
    // Copies `from` to `part` (a new file) and checks the copy (SHA-256), with `from`'s quarantine attribute; removes
    // the part when anything fails. Leaves `from` as it is.
    FileResult copyToPart(const QString &from, const QString &part) const;
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
