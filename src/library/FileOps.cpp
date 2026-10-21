// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/FileOps.h"

#include "library/Quarantine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::library {
namespace {

Q_LOGGING_CATEGORY(lcFileOps, "selecta.library.fileops")

constexpr int kMaxNameVariants = 999;
constexpr qint64 kCopyChunk = qint64{1024} * 1024;

// Canonical form of a path that may not exist yet: the canonical path of its nearest existing ancestor plus the rest.
QString resolved(const QString &path)
{
    QString cleaned = QDir::cleanPath(QDir(path).absolutePath());
    QString existing = cleaned;
    QString rest;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const qsizetype slash = existing.lastIndexOf(u'/');
        if (slash <= 0) {
            return cleaned;
        }
        rest = existing.mid(slash) + rest;
        existing = existing.left(slash);
    }
    const QString canonical = QFileInfo(existing).canonicalFilePath();
    return canonical.isEmpty() ? cleaned : canonical + rest;
}

// "name.mp3" → "name (2).mp3"; folders and files without an extension get the suffix at the end.
QString variant(const QString &path, int n)
{
    const QFileInfo info(path);
    const QString suffix = info.isDir() ? QString() : info.completeSuffix();
    const QString base = suffix.isEmpty() ? info.fileName() : info.fileName().chopped(suffix.size() + 1);
    const QString name =
        suffix.isEmpty() ? u"%1 (%2)"_s.arg(base).arg(n) : u"%1 (%2).%3"_s.arg(base).arg(n).arg(suffix);
    return info.dir().filePath(name);
}

QByteArray sha256(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = u"can't read %1: %2"_s.arg(path, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        *error = u"can't read %1"_s.arg(path);
        return {};
    }
    return hash.result();
}

// Renames `from` to `to` only if `to` doesn't exist, atomically where the OS allows it. Returns 0 or an errno value.
int renameExclusive(const QString &from, const QString &to)
{
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
#ifdef Q_OS_MACOS
    return renamex_np(source.constData(), target.constData(), RENAME_EXCL) == 0 ? 0 : errno;
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    constexpr unsigned kNoReplace = 1; // RENAME_NOREPLACE
    return syscall(SYS_renameat2, AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), kNoReplace) == 0 ? 0
                                                                                                               : errno;
#else
    if (QFileInfo::exists(to)) {
        return EEXIST;
    }
    return std::rename(source.constData(), target.constData()) == 0 ? 0 : errno;
#endif
}

// The message strerror_r or strerror_s left: the XSI and Windows ones return 0 and fill the buffer, glibc's GNU
// strerror_r returns the message itself. Null when there is none.
[[maybe_unused]] const char *errorMessage(int result, const char *buffer)
{
    return result == 0 ? buffer : nullptr;
}

[[maybe_unused]] const char *errorMessage(const char *result, const char * /*buffer*/)
{
    return result;
}

QString describeErrno(int code)
{
    std::array<char, 256> buffer{};
#ifdef Q_OS_WIN
    const char *message = errorMessage(strerror_s(buffer.data(), buffer.size(), code), buffer.data());
#else
    const char *message = errorMessage(strerror_r(code, buffer.data(), buffer.size()), buffer.data());
#endif
    if (message == nullptr || *message == '\0') {
        return u"error %1"_s.arg(code);
    }
    return QString::fromLocal8Bit(message);
}

// Everything in the Inbox came from a peer (D20), so it carries the quarantine attribute before it appears there
// (NFR-4); an attribute it already has stays. A volume that can't take one doesn't stop the file: the log says so.
void quarantineDownload(const QString &path, bool folder)
{
    QString why;
    if (!(folder ? quarantine::markTree(path, &why) : quarantine::mark(path, &why))) {
        qCWarning(lcFileOps).noquote() << "no quarantine attribute:" << why;
    }
}

// A copy made by reading and writing has none of the original's extended attributes: it gets the quarantine back.
void keepQuarantine(const QString &from, const QString &to)
{
    QString why;
    if (!quarantine::copy(from, to, &why)) {
        qCWarning(lcFileOps).noquote() << "the copy lost its quarantine attribute:" << why;
    }
}

} // namespace

bool moveToSystemTrash(const QString &path, QString *trashedPath)
{
    QString pathInTrash;
    const bool ok = QFile::moveToTrash(path, &pathInTrash);
    if (trashedPath) {
        *trashedPath = std::move(pathInTrash);
    }
    return ok;
}

FileOps::FileOps(const QString &inboxRoot, const QStringList &collectionRoots, TrashFunction trash)
    : m_inboxRoot(resolved(inboxRoot))
    , m_trash(trash ? std::move(trash) : TrashFunction(&moveToSystemTrash))
{
    m_collectionRoots.reserve(collectionRoots.size());
    for (const QString &root : collectionRoots) {
        m_collectionRoots.append(resolved(root));
    }
}

bool FileOps::isInside(const QString &path, const QString &root)
{
    const QString file = resolved(path);
    const QString base = resolved(root);
    return !base.isEmpty() && file.size() > base.size() && file.startsWith(base) && file.at(base.size()) == u'/';
}

FileResult FileOps::moveExclusive(const QString &from, const QString &to) const
{
    if (!m_forceCopy) {
        const int code = renameExclusive(from, to);
        if (code == 0) {
            return FileResult::success(to);
        }
        if (code != EXDEV) {
            return FileResult::failure(u"can't move %1 to %2: %3"_s.arg(from, to, describeErrno(code)));
        }
    }
    return copyVerifyRename(from, to);
}

FileResult FileOps::copyVerifyRename(const QString &from, const QString &to) const
{
    QString error;
    const QByteArray sourceHash = sha256(from, &error);
    if (sourceHash.isEmpty()) {
        return FileResult::failure(error);
    }
    // Write next to the target, then rename into place, so the target name only ever holds a complete, checked copy.
    const QString part = to + u".selecta-part"_s;
    QFile::remove(part);
    {
        QFile source(from);
        QFile target(part);
        if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return FileResult::failure(u"can't copy %1: %2%3"_s.arg(from, source.errorString(), target.errorString()));
        }
        while (!source.atEnd()) {
            const QByteArray chunk = source.read(kCopyChunk);
            if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
                target.remove();
                return FileResult::failure(u"read error in %1: %2"_s.arg(from, source.errorString()));
            }
            if (target.write(chunk) != chunk.size()) {
                const QString reason = target.errorString();
                target.remove();
                return FileResult::failure(u"write error in %1: %2"_s.arg(part, reason));
            }
        }
        if (!target.flush()) {
            target.remove();
            return FileResult::failure(u"can't flush %1"_s.arg(part));
        }
    }
    if (sha256(part, &error) != sourceHash) {
        QFile::remove(part);
        return FileResult::failure(error.isEmpty() ? u"the copy of %1 doesn't match the original"_s.arg(from) : error);
    }
    keepQuarantine(from, part);
    if (const int code = renameExclusive(part, to); code != 0) {
        QFile::remove(part);
        return FileResult::failure(u"can't create %1: %2"_s.arg(to, describeErrno(code)));
    }
    if (!QFile::remove(from)) {
        // The new file is complete and verified; a leftover source is safe, so report success with a note.
        return FileResult{true, to, u"copied, but the original stayed at %1"_s.arg(from)};
    }
    return FileResult::success(to);
}

FileResult FileOps::publishIntoInbox(const QString &sourceFile, const QString &relativePath) const
{
    if (relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)) {
        return FileResult::failure(u"the Inbox path must be relative: %1"_s.arg(relativePath));
    }
    const QString target = QDir::cleanPath(m_inboxRoot + u'/' + relativePath);
    if (!isInside(target, m_inboxRoot)) {
        return FileResult::failure(u"refusing to write outside the Inbox: %1"_s.arg(relativePath));
    }
    if (!QFileInfo(sourceFile).isFile()) {
        return FileResult::failure(u"nothing to publish at %1"_s.arg(sourceFile));
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        return FileResult::failure(u"can't create the folder for %1"_s.arg(target));
    }
    quarantineDownload(sourceFile, false);
    for (int n = 1; n <= kMaxNameVariants; ++n) {
        const QString candidate = n == 1 ? target : variant(target, n);
        if (QFileInfo::exists(candidate)) {
            continue;
        }
        FileResult result = moveExclusive(sourceFile, candidate);
        // Lost a race for this name (it appeared meanwhile): try the next one. Anything else is a real failure.
        if (!result.ok && QFileInfo::exists(candidate) && QFileInfo::exists(sourceFile)) {
            continue;
        }
        return result;
    }
    return FileResult::failure(u"too many files named like %1 in the Inbox"_s.arg(target));
}

FileResult FileOps::placeNewFile(const QString &stagedFile, const QString &targetPath) const
{
    if (!QFileInfo(stagedFile).isFile()) {
        return FileResult::failure(u"nothing to place at %1"_s.arg(stagedFile));
    }
    if (targetPath.isEmpty() || !QDir::isAbsolutePath(targetPath)) {
        return FileResult::failure(u"the new file's path must be absolute: %1"_s.arg(targetPath));
    }
    const QString target = QDir::cleanPath(targetPath);
    if (!QFileInfo(QFileInfo(target).absolutePath()).isDir()) {
        return FileResult::failure(u"there's no folder %1"_s.arg(QFileInfo(target).absolutePath()));
    }
    for (int n = 1; n <= kMaxNameVariants; ++n) {
        const QString candidate = n == 1 ? target : variant(target, n);
        if (QFileInfo::exists(candidate)) {
            continue;
        }
        FileResult result = moveExclusive(stagedFile, candidate);
        // Lost a race for this name (it appeared meanwhile): try the next one. Anything else is a real failure.
        if (!result.ok && QFileInfo::exists(candidate) && QFileInfo::exists(stagedFile)) {
            continue;
        }
        return result;
    }
    return FileResult::failure(u"too many files named like %1"_s.arg(target));
}

FileResult FileOps::collectionTarget(const QString &collectionRoot, const QString &relativePath) const
{
    const QString root = resolved(collectionRoot);
    if (!m_collectionRoots.contains(root)) {
        return FileResult::failure(u"%1 isn't a collection folder"_s.arg(collectionRoot));
    }
    if (relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)) {
        return FileResult::failure(u"the collection path must be relative: %1"_s.arg(relativePath));
    }
    const QString target = QDir::cleanPath(root + u'/' + relativePath);
    if (!isInside(target, root)) {
        return FileResult::failure(u"refusing to write outside the collection: %1"_s.arg(relativePath));
    }
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
        return FileResult::failure(u"%1 already exists; Selecta never overwrites collection files"_s.arg(target));
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        return FileResult::failure(u"can't create the folder for %1"_s.arg(target));
    }
    return FileResult::success(target);
}

FileResult FileOps::fileIntoCollection(const QString &inboxFile, const QString &collectionRoot,
                                       const QString &relativePath) const
{
    if (!isInside(inboxFile, m_inboxRoot) || !QFileInfo(inboxFile).isFile()) {
        return FileResult::failure(u"only Inbox files can be filed: %1"_s.arg(inboxFile));
    }
    FileResult target = collectionTarget(collectionRoot, relativePath);
    if (!target.ok) {
        return target;
    }
    return moveExclusive(inboxFile, target.path);
}

FileResult FileOps::fileFolderIntoCollection(const QString &inboxFolder, const QString &collectionRoot,
                                             const QString &relativeFolder) const
{
    const QFileInfo folder(inboxFolder);
    if (!isInside(inboxFolder, m_inboxRoot) || !folder.isDir() || folder.isSymLink()) {
        return FileResult::failure(u"only Inbox folders can be filed: %1"_s.arg(inboxFolder));
    }
    // Only plain files and folders: a symbolic link could point anywhere, including into the collection.
    QDirIterator entries(inboxFolder, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QFileInfo entry = entries.nextFileInfo();
        if (entry.isSymLink() || (!entry.isFile() && !entry.isDir())) {
            return FileResult::failure(
                u"%1 contains %2, which isn't a plain file"_s.arg(inboxFolder, entry.filePath()));
        }
    }
    FileResult target = collectionTarget(collectionRoot, relativeFolder);
    if (!target.ok) {
        return target;
    }
    if (!m_forceCopy) {
        const int code = renameExclusive(resolved(inboxFolder), target.path);
        if (code == 0) {
            return FileResult::success(target.path);
        }
        if (code != EXDEV) {
            return FileResult::failure(u"can't move %1 to %2: %3"_s.arg(inboxFolder, target.path, describeErrno(code)));
        }
    }
    return copyFolderVerified(resolved(inboxFolder), target.path);
}

FileResult FileOps::copyFolderVerified(const QString &from, const QString &to) const
{
    // A hidden staging folder next to the target, named after it: a retry after a crash finds and replaces its own
    // leftovers, and nothing else in the collection is ever touched.
    const QFileInfo targetInfo(to);
    const QString stage = targetInfo.dir().filePath(u"."_s + targetInfo.fileName() + u".selecta-part"_s);
    if (QFileInfo::exists(stage) && !QDir(stage).removeRecursively()) {
        return FileResult::failure(u"can't clear the leftover %1"_s.arg(stage));
    }
    if (!QDir().mkdir(stage)) {
        return FileResult::failure(u"can't create %1"_s.arg(stage));
    }
    const auto abandon = [&stage](const QString &error) {
        QDir(stage).removeRecursively();
        return FileResult::failure(error);
    };

    QDirIterator files(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString source = files.next();
        const QString relative = QDir(from).relativeFilePath(source);
        const QString copy = stage + u'/' + relative;
        if (!QDir().mkpath(QFileInfo(copy).absolutePath())) {
            return abandon(u"can't create the folder for %1"_s.arg(copy));
        }
        QString error;
        const QByteArray sourceHash = sha256(source, &error);
        if (sourceHash.isEmpty() || !QFile::copy(source, copy)) {
            return abandon(error.isEmpty() ? u"can't copy %1"_s.arg(source) : error);
        }
        if (sha256(copy, &error) != sourceHash) {
            return abandon(error.isEmpty() ? u"the copy of %1 doesn't match the original"_s.arg(source) : error);
        }
        keepQuarantine(source, copy);
    }
    // The folders keep their quarantine attribute too.
    keepQuarantine(from, stage);
    QDirIterator folders(from, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDirIterator::Subdirectories);
    while (folders.hasNext()) {
        const QString folder = folders.next();
        if (const QString copy = stage + u'/' + QDir(from).relativeFilePath(folder); QFileInfo(copy).isDir()) {
            keepQuarantine(folder, copy);
        }
    }
    if (const int code = renameExclusive(stage, to); code != 0) {
        return abandon(u"can't create %1: %2"_s.arg(to, describeErrno(code)));
    }
    if (!QDir(from).removeRecursively()) {
        // The album is complete and verified in the collection; leftovers in the Inbox are safe.
        return FileResult{true, to, u"copied, but some of the originals stayed in %1"_s.arg(from)};
    }
    return FileResult::success(to);
}

FileResult FileOps::trashFromInbox(const QString &path) const
{
    if (!isInside(path, m_inboxRoot)) {
        return FileResult::failure(u"only Inbox items can be trashed by Selecta: %1"_s.arg(path));
    }
    if (!QFileInfo::exists(path)) {
        return FileResult::failure(u"nothing at %1"_s.arg(path));
    }
    QString trashed;
    if (!m_trash(resolved(path), &trashed)) {
        return FileResult::failure(u"couldn't move %1 to the Trash"_s.arg(path));
    }
    return FileResult::success(trashed);
}

FileResult FileOps::publishFolderIntoInbox(const QString &sourceFolder, const QString &relativeFolder) const
{
    if (relativeFolder.isEmpty() || QDir::isAbsolutePath(relativeFolder)) {
        return FileResult::failure(u"the Inbox path must be relative: %1"_s.arg(relativeFolder));
    }
    const QString target = QDir::cleanPath(m_inboxRoot + u'/' + relativeFolder);
    if (!isInside(target, m_inboxRoot) || target == QDir::cleanPath(m_inboxRoot)) {
        return FileResult::failure(u"refusing to write outside the Inbox: %1"_s.arg(relativeFolder));
    }
    const QFileInfo source(sourceFolder);
    if (!source.isDir() || source.isSymLink()) {
        return FileResult::failure(u"nothing to publish at %1"_s.arg(sourceFolder));
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        return FileResult::failure(u"can't create the folder for %1"_s.arg(target));
    }
    quarantineDownload(sourceFolder, true);
    for (int n = 1; n <= kMaxNameVariants; ++n) {
        // Folder names may hold dots ("Mr. Oizo - Analog Worms Attack"): the number goes at the end.
        const QString candidate = n == 1 ? target : u"%1 (%2)"_s.arg(target).arg(n);
        if (QFileInfo::exists(candidate)) {
            continue;
        }
        if (!m_forceCopy) {
            const int code = renameExclusive(sourceFolder, candidate);
            if (code == 0) {
                return FileResult::success(candidate);
            }
            if (code == EEXIST) {
                continue; // appeared meanwhile
            }
            if (code != EXDEV) {
                return FileResult::failure(
                    u"can't move %1 to %2: %3"_s.arg(sourceFolder, candidate, describeErrno(code)));
            }
        }
        FileResult copied = copyFolderVerified(sourceFolder, candidate);
        if (copied.ok) {
            QDir(sourceFolder).removeRecursively();
        }
        return copied;
    }
    return FileResult::failure(u"too many folders named like %1 in the Inbox"_s.arg(target));
}

FileResult FileOps::checkReplace(const QString &inboxFile, const QString &collectionFile) const
{
    const QFileInfo source(inboxFile);
    if (!isInside(inboxFile, m_inboxRoot) || !source.isFile() || source.isSymLink()) {
        return FileResult::failure(u"only Inbox files can be filed: %1"_s.arg(inboxFile));
    }
    if (collectionFile.isEmpty() || !QDir::isAbsolutePath(collectionFile)) {
        return FileResult::failure(u"the collection file's path must be absolute: %1"_s.arg(collectionFile));
    }
    if (QFileInfo(QDir::cleanPath(collectionFile)).isSymLink()) {
        return FileResult::failure(u"%1 is a symbolic link; Selecta won't replace it"_s.arg(collectionFile));
    }
    // As the collection's other filed paths are reported: real ones.
    const QString target = resolved(collectionFile);
    const bool inCollection =
        std::ranges::any_of(m_collectionRoots, [&target](const QString &root) { return isInside(target, root); });
    if (!inCollection) {
        return FileResult::failure(u"%1 isn't in a collection folder"_s.arg(collectionFile));
    }
    const QFileInfo old(target);
    if (!QFileInfo(old.absolutePath()).isDir()) {
        return FileResult::failure(u"there's no folder %1"_s.arg(old.absolutePath()));
    }
    if (old.exists() && !old.isFile()) {
        return FileResult::failure(u"%1 isn't a plain file; Selecta won't replace it"_s.arg(target));
    }
    if (source.suffix().compare(old.suffix(), Qt::CaseInsensitive) != 0) {
        return FileResult::failure(u"%1 and %2 aren't of the same type"_s.arg(source.fileName(), old.fileName()));
    }
    return FileResult::success(target);
}

FileResult FileOps::replaceInCollection(const QString &inboxFile, const QString &collectionFile) const
{
    const FileResult checked = checkReplace(inboxFile, collectionFile);
    if (!checked.ok) {
        return checked;
    }
    const QString &target = checked.path;
    const QFileInfo old(target);

    // A verified copy, hidden next to the old file (the collection's scans skip it); the Inbox file stays meanwhile.
    QString error;
    const QByteArray sourceHash = sha256(inboxFile, &error);
    if (sourceHash.isEmpty()) {
        return FileResult::failure(error);
    }
    const QString staged = old.dir().filePath(u"."_s + old.fileName() + u".selecta-part"_s);
    QFile::remove(staged); // left by an attempt a crash cut short
    if (!QFile::copy(inboxFile, staged)) {
        QFile::remove(staged);
        return FileResult::failure(u"can't copy %1 next to %2"_s.arg(inboxFile, target));
    }
    if (sha256(staged, &error) != sourceHash) {
        QFile::remove(staged);
        return FileResult::failure(error.isEmpty() ? u"the copy of %1 doesn't match the original"_s.arg(inboxFile)
                                                   : error);
    }
    keepQuarantine(inboxFile, staged);

    // The old file to the Trash, then the copy in its place.
    QString trashed;
    if (old.exists() && !m_trash(target, &trashed)) {
        QFile::remove(staged);
        return FileResult::failure(u"couldn't move %1 to the Trash; nothing changed"_s.arg(target));
    }
    if (const int code = renameExclusive(staged, target); code != 0) {
        QFile::remove(staged);
        QString outcome;
        if (trashed.isEmpty()) {
            outcome = u"nothing changed"_s;
        } else if (renameExclusive(trashed, target) == 0) {
            outcome = u"the old file is back"_s;
        } else {
            outcome = u"the old file is in the Trash: %1"_s.arg(trashed);
        }
        return FileResult::failure(u"can't put the new file at %1 (%2); %3"_s.arg(target, describeErrno(code), outcome));
    }
    qCInfo(lcFileOps).noquote() << "replaced" << target
                                << (trashed.isEmpty() ? u"(no old file)"_s : u"(the old file: "_s + trashed + u')');
    if (!QFile::remove(inboxFile)) {
        // The new file is in place and verified; a leftover Inbox copy is safe, so report success with a note.
        return FileResult{true, target, u"replaced, but the Inbox copy stayed at %1"_s.arg(inboxFile)};
    }
    return FileResult::success(target);
}

FileResult FileOps::replaceInInbox(const QString &newFile, const QString &inboxFile) const
{
    if (!isInside(newFile, m_inboxRoot) || !isInside(inboxFile, m_inboxRoot)) {
        return FileResult::failure(u"only Inbox files can be replaced by Selecta: %1"_s.arg(inboxFile));
    }
    if (!QFileInfo(newFile).isFile() || !QFileInfo(inboxFile).isFile()) {
        return FileResult::failure(u"nothing to replace at %1"_s.arg(inboxFile));
    }
    // The new copy is the same download: it takes the file's quarantine attribute, or gets one.
    keepQuarantine(inboxFile, newFile);
    quarantineDownload(newFile, false);
    std::error_code error;
    std::filesystem::rename(std::filesystem::path(newFile.toStdU16String()),
                            std::filesystem::path(inboxFile.toStdU16String()), error); // rename(2): atomic
    if (error) {
        return FileResult::failure(
            u"couldn't replace %1: %2"_s.arg(inboxFile, QString::fromLocal8Bit(error.message())));
    }
    return FileResult::success(inboxFile);
}

} // namespace selecta::library
