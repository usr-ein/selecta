// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"

#include <QByteArray>
#include <QString>

#include <sys/stat.h>

// Internal: opening a shared file only if it is still the file that was indexed (sharing note §7). Used by the
// build's header probes and by share::open(). Reentrant; any thread.
namespace selecta::share::detail {

// The identity a file had when it was walked.
struct FileIdentity
{
    quint64 device = 0;
    quint64 inode = 0;
    quint64 size = 0;
};

enum class OpenFailure {
    None,
    Gone,       // nothing at the path any more (ENOENT, ENOTDIR)
    Replaced,   // a symlink now (ELOOP under O_NOFOLLOW), another device or inode, not a regular file
    Changed,    // the same file with another size
    NotOnDisk,  // an iCloud placeholder: its data isn't on this Mac
    Unreadable, // any other failure of open(2) or fstat(2): permissions, I/O, no descriptors left
};

// What fstat(2) says about an open file, against the identity it should have.
OpenFailure verify(const struct stat &status, const FileIdentity &expected);

struct CheckedDescriptor
{
    int fd = -1; // read-only, blocking, close-on-exec; the caller owns it. -1 on failure.
    OpenFailure failure = OpenFailure::None;
    int error = 0; // errno of the failed call, 0 when the checks failed
};

// open(2) with O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC (a FIFO swapped in can't block the open), then verify()
// on fstat(2), then O_NONBLOCK cleared. `nativePath` is used byte for byte. Never reads.
CheckedDescriptor openChecked(const QByteArray &nativePath, const FileIdentity &expected);

// A log line for a failure ("its size changed since it was indexed"), with the errno text when there is one.
QString describe(OpenFailure failure, int error);

// A complete file read with pread(2) through a descriptor openChecked() returned: what the build's header probes read
// from. Thread-safe like every ByteSource; owns and closes the descriptor.
class DescriptorSource final : public audio::ByteSource
{
public:
    DescriptorSource(int fd, qint64 size, QString description);
    ~DescriptorSource() override;
    DescriptorSource(const DescriptorSource &) = delete;
    DescriptorSource &operator=(const DescriptorSource &) = delete;
    DescriptorSource(DescriptorSource &&) = delete;
    DescriptorSource &operator=(DescriptorSource &&) = delete;

    qint64 totalSize() const override { return m_size; }
    audio::SourceSnapshot snapshot() const override { return {m_size, audio::SourceState::Complete}; }
    // Exact reads, as audio::FileSource's: a short read (the file shrank) is an error.
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override;
    QString description() const override { return m_description; }

private:
    int m_fd = -1;
    qint64 m_size = 0;
    QString m_description;
};

} // namespace selecta::share::detail
