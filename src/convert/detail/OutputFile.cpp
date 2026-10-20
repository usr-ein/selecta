// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/detail/OutputFile.h"

#include "convert/MetadataModel.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStorageInfo>

#include <cerrno>
#include <cstdio>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#endif
#ifdef Q_OS_WIN
#include <io.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::convert::detail {
namespace {

constexpr qint64 kLowSpaceBytes = qint64{1024} * 1024;
constexpr qsizetype kMaxNameBytes = 255; // one path component on APFS, HFS+, ext4 and NTFS (in UTF-16 units there)
constexpr int kCreateAttempts = 16;

// `name` shortened (whole characters) so that its UTF-8 form is at most `maxBytes` long.
QString truncatedToBytes(QString name, qsizetype maxBytes)
{
    while (!name.isEmpty() && name.toUtf8().size() > maxBytes) {
        name.chop(name.size() >= 2 && name.back().isLowSurrogate() ? 2 : 1);
    }
    return name;
}

Error errnoError(int code, const QString &what)
{
    if (code == ENOSPC) {
        return audio::makeError(ErrorCode::DiskFull, u"the disk is full: %1"_s.arg(what));
    }
    return audio::makeError(ErrorCode::WriteFailed, u"%1: %2"_s.arg(what, qt_error_string(code)));
}

#ifdef Q_OS_UNIX
// link() + unlink(): fails with EEXIST rather than replacing. Returns 0 or an errno value.
int linkThenUnlink(const QByteArray &source, const QByteArray &target)
{
    if (::link(source.constData(), target.constData()) != 0) {
        return errno;
    }
    if (::unlink(source.constData()) != 0) {
        qCWarning(lcConvert) << "published" << target << "but couldn't remove" << source;
    }
    return 0;
}
#endif

} // namespace

QString noSpaceMessage()
{
    return qt_error_string(ENOSPC);
}

Error writeError(const QIODevice &device, const QString &path)
{
    const QString reason = device.errorString();
    bool full = reason == noSpaceMessage();
#ifdef EDQUOT
    full = full || reason == qt_error_string(EDQUOT);
#endif
    if (!full) {
        const QStorageInfo storage(QFileInfo(path).absolutePath());
        full = storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < kLowSpaceBytes;
    }
    if (full) {
        return audio::makeError(ErrorCode::DiskFull, u"the disk is full: can't write %1"_s.arg(path));
    }
    return audio::makeError(ErrorCode::WriteFailed, u"can't write %1: %2"_s.arg(path, reason));
}

// ---------------------------------------------------------------------------------------------------------------
// Sink

Sink::Sink(QIODevice *device, QString path)
    : m_device(device)
    , m_path(std::move(path))
{}

Result<void> Sink::fail(Error error)
{
    if (!m_error.isError()) {
        m_error = std::move(error);
    }
    return m_error;
}

Result<void> Sink::write(const void *data, qint64 size)
{
    if (m_error.isError()) {
        return m_error;
    }
    if (size <= 0) {
        return {};
    }
    const qint64 written = m_device->write(static_cast<const char *>(data), size);
    if (written != size) {
        return fail(writeError(*m_device, m_path));
    }
    return {};
}

Result<void> Sink::seek(qint64 position)
{
    if (m_error.isError()) {
        return m_error;
    }
    if (!m_device->seek(position)) {
        return fail(
            audio::makeError(ErrorCode::WriteFailed, u"can't seek in %1: %2"_s.arg(m_path, m_device->errorString())));
    }
    return {};
}

qint64 Sink::position() const
{
    return m_device->pos();
}

qint64 Sink::size() const
{
    return m_device->size();
}

// ---------------------------------------------------------------------------------------------------------------
// renameExclusive

Result<void> renameExclusive(const QString &from, const QString &to)
{
    const QString what = u"can't move %1 to %2"_s.arg(from, to);
#ifdef Q_OS_UNIX
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
#ifdef Q_OS_DARWIN
    int code = ::renamex_np(source.constData(), target.constData(), RENAME_EXCL) == 0 ? 0 : errno;
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    constexpr unsigned kNoReplace = 1; // RENAME_NOREPLACE
    int code = ::syscall(SYS_renameat2, AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), kNoReplace) == 0
                   ? 0
                   : errno;
#else
    int code = ENOTSUP;
#endif
    // File systems without an exclusive rename (some network and FUSE volumes) refuse it; link() never replaces.
    if (code == ENOTSUP || code == EINVAL || code == ENOSYS) {
        code = linkThenUnlink(source, target);
    }
    if (code == EEXIST) {
        return audio::makeError(ErrorCode::OutputExists, u"%1 already exists"_s.arg(to));
    }
    if (code != 0) {
        return errnoError(code, what);
    }
    return {};
#else
    // Windows: MoveFileEx without MOVEFILE_REPLACE_EXISTING, which fails when the target exists.
    if (QFileInfo::exists(to)) {
        return audio::makeError(ErrorCode::OutputExists, u"%1 already exists"_s.arg(to));
    }
    QFile file(from);
    if (!file.rename(to)) {
        return QFileInfo::exists(to)
                   ? audio::makeError(ErrorCode::OutputExists, u"%1 already exists"_s.arg(to))
                   : audio::makeError(ErrorCode::WriteFailed, u"%1: %2"_s.arg(what, file.errorString()));
    }
    return {};
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// TempOutput

TempOutput::TempOutput(QString path, QString finalPath)
    : m_path(std::move(path))
    , m_finalPath(std::move(finalPath))
    , m_file(m_path)
{}

TempOutput::~TempOutput()
{
    if (!m_created || m_published) {
        return;
    }
    m_file.close();
    if (QFile::exists(m_path) && !QFile::remove(m_path)) {
        qCWarning(lcConvert) << "couldn't delete the temporary file" << m_path;
    }
}

Result<std::unique_ptr<TempOutput>> TempOutput::create(const QString &finalPath)
{
    if (finalPath.isEmpty()) {
        return audio::makeError(ErrorCode::InvalidArgument, u"no output path"_s);
    }
    const QFileInfo info(finalPath);
    if (info.exists() || info.isSymLink()) {
        return audio::makeError(ErrorCode::OutputExists, u"%1 already exists"_s.arg(finalPath));
    }
    const QString name = info.fileName();
    if (name.isEmpty()) {
        return audio::makeError(ErrorCode::InvalidArgument, u"%1 names a folder, not a file"_s.arg(finalPath));
    }
    const QDir folder = info.absoluteDir();
    if (!folder.exists()) {
        return audio::makeError(ErrorCode::OpenFailed, u"the folder of %1 doesn't exist"_s.arg(finalPath));
    }
    QString lastError;
    for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
        const QString random = u"%1"_s.arg(QRandomGenerator::global()->generate(), 8, 16, u'0');
        const QString suffix = u'.' + random + kPartSuffix.toString();
        const QString base = truncatedToBytes(name, kMaxNameBytes - suffix.toUtf8().size());
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by the unique_ptr at once
        std::unique_ptr<TempOutput> output(new TempOutput(folder.filePath(base + suffix), info.absoluteFilePath()));
        if (output->m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered)) {
            output->m_created = true;
            return output;
        }
        lastError = output->m_file.errorString();
        if (!QFileInfo::exists(output->m_path)) {
            break; // not a name collision: the folder isn't writable
        }
    }
    return audio::makeError(ErrorCode::OpenFailed,
                            u"can't create a temporary file next to %1: %2"_s.arg(finalPath, lastError));
}

Result<void> TempOutput::closeFile()
{
    if (!m_file.isOpen()) {
        return {};
    }
    m_file.close();
    if (m_file.error() != QFileDevice::NoError) {
        return writeError(m_file, m_path);
    }
    return {};
}

Result<void> TempOutput::sync()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return audio::makeError(ErrorCode::WriteFailed, u"can't reopen %1: %2"_s.arg(m_path, file.errorString()));
    }
#ifdef Q_OS_UNIX
    if (::fsync(file.handle()) != 0) {
        return errnoError(errno, u"can't flush %1 to the disk"_s.arg(m_path));
    }
#elif defined(Q_OS_WIN)
    if (file.handle() >= 0 && ::_commit(file.handle()) != 0) {
        return errnoError(errno, u"can't flush %1 to the disk"_s.arg(m_path));
    }
#endif
    return {};
}

Result<void> TempOutput::publish()
{
    if (m_published) {
        return audio::makeError(ErrorCode::Internal, u"%1 was already published"_s.arg(m_finalPath));
    }
    if (const Result<void> closed = closeFile(); !closed.ok()) {
        return closed;
    }
    if (const Result<void> renamed = renameExclusive(m_path, m_finalPath); !renamed.ok()) {
        return renamed;
    }
    m_published = true;
    return {};
}

} // namespace selecta::convert::detail
