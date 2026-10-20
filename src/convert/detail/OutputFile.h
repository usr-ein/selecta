// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"

#include <QFile>
#include <QIODevice>
#include <QString>

#include <memory>

// Writing a conversion's output: the temporary file next to the final path, the byte sink encoders write through, and
// the no-overwrite publication (conversion §2.5, ADR-009).
namespace selecta::convert::detail {

using audio::Error;
using audio::ErrorCode;
template <typename T>
using Result = audio::Result<T>;

// Suffix of every temporary output ("<name>.<random>.selecta-part"); core may sweep stale ones after a crash.
inline constexpr QStringView kPartSuffix = u".selecta-part";

// The message Qt gives a write that failed for lack of space; a simulated full disk reports the same.
QString noSpaceMessage();

// The error for a failed write on `device`: DiskFull when the device says there is no space left (ENOSPC, EDQUOT) or
// the volume holding `path` has less than 1 MiB free, else WriteFailed.
Error writeError(const QIODevice &device, const QString &path);

// The bytes an encoder writes: a QIODevice plus error classification. The first failure is kept (error()).
// Not thread-safe.
class Sink
{
public:
    Sink(QIODevice *device, QString path);

    // Writes every byte or fails with DiskFull or WriteFailed.
    Result<void> write(const void *data, qint64 size);
    Result<void> seek(qint64 position);
    qint64 position() const;
    qint64 size() const;
    const QString &path() const { return m_path; }
    // The first failure, or an Error with ErrorCode::None.
    const Error &error() const { return m_error; }

private:
    Result<void> fail(Error error);

    QIODevice *m_device = nullptr;
    QString m_path;
    Error m_error;
};

// Renames `from` to `to` only if `to` doesn't exist, atomically where the system allows it: renamex_np(RENAME_EXCL)
// on macOS, renameat2(RENAME_NOREPLACE) on Linux, link() + unlink() on other Unix systems (and where those calls are
// refused), MoveFileEx without replacing on Windows. Failure codes: OutputExists, WriteFailed.
Result<void> renameExclusive(const QString &from, const QString &to);

// The temporary file a conversion writes, "<name>.<random>.selecta-part" in the final path's folder (same volume, so
// publishing is a rename). The file is deleted when the object is destroyed unless it was published.
//
// Threading: one thread at a time.
class TempOutput
{
public:
    // Creates the temporary file (exclusively) and opens it for writing. Failure codes: InvalidArgument (empty path),
    // OutputExists (finalPath exists), OpenFailed (no such folder, or it isn't writable).
    static Result<std::unique_ptr<TempOutput>> create(const QString &finalPath);

    ~TempOutput();
    TempOutput(const TempOutput &) = delete;
    TempOutput &operator=(const TempOutput &) = delete;
    TempOutput(TempOutput &&) = delete;
    TempOutput &operator=(TempOutput &&) = delete;

    const QString &path() const { return m_path; }
    const QString &finalPath() const { return m_finalPath; }
    // Open for writing (unbuffered, so failures surface at the write that caused them) until closeFile().
    QFile &file() { return m_file; }
    // Closes the file. Failure codes: DiskFull, WriteFailed.
    Result<void> closeFile();
    // Flushes the file's data to the disk (fsync), so a crash after publishing can't leave a hole. Failure codes:
    // DiskFull, WriteFailed.
    Result<void> sync();
    // Renames the file to the final path without replacing anything. Failure codes: OutputExists (the final path
    // appeared meanwhile), WriteFailed.
    Result<void> publish();

private:
    TempOutput(QString path, QString finalPath);

    QString m_path;
    QString m_finalPath;
    QFile m_file;
    bool m_created = false; // the file is ours to delete
    bool m_published = false;
};

} // namespace selecta::convert::detail
