// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/Descriptor.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace selecta::share::detail {

namespace {

bool isDataless([[maybe_unused]] const struct stat &status)
{
#ifdef SF_DATALESS
    return (status.st_flags & SF_DATALESS) != 0;
#else
    return false;
#endif
}

void closeQuietly(int fd)
{
    // A descriptor opened read-only that was never written: close() has nothing to report.
    (void)::close(fd);
}

} // namespace

OpenFailure verify(const struct stat &status, const FileIdentity &expected)
{
    if (!S_ISREG(status.st_mode) || static_cast<quint64>(status.st_dev) != expected.device ||
        static_cast<quint64>(status.st_ino) != expected.inode) {
        return OpenFailure::Replaced;
    }
    if (status.st_size < 0 || static_cast<quint64>(status.st_size) != expected.size) {
        return OpenFailure::Changed;
    }
    if (isDataless(status)) {
        return OpenFailure::NotOnDisk;
    }
    return OpenFailure::None;
}

CheckedDescriptor openChecked(const QByteArray &nativePath, const FileIdentity &expected)
{
    CheckedDescriptor result;
    int fd = -1;
    do {
        fd = ::open(nativePath.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); // NOLINT(*-vararg)
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        result.error = errno;
        switch (result.error) {
        case ENOENT:
        case ENOTDIR:
            result.failure = OpenFailure::Gone;
            break;
        case ELOOP:
            result.failure = OpenFailure::Replaced;
            break;
        default:
            result.failure = OpenFailure::Unreadable;
            break;
        }
        return result;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        result.error = errno;
        result.failure = OpenFailure::Unreadable;
        closeQuietly(fd);
        return result;
    }
    result.failure = verify(status, expected);
    if (result.failure != OpenFailure::None) {
        closeQuietly(fd);
        return result;
    }
    // Reads of a regular file never block on O_NONBLOCK's account, but whoever reads later expects a plain descriptor.
    const int flags = ::fcntl(fd, F_GETFL); // NOLINT(*-vararg)
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) { // NOLINT(*-vararg, *-signed-bitwise)
        result.error = errno;
        result.failure = OpenFailure::Unreadable;
        closeQuietly(fd);
        return result;
    }
    result.fd = fd;
    return result;
}

QString describe(OpenFailure failure, int error)
{
    QString text;
    switch (failure) {
    case OpenFailure::None:
        return {};
    case OpenFailure::Gone:
        text = u"it is gone"_s;
        break;
    case OpenFailure::Replaced:
        text = u"it was replaced since it was indexed"_s;
        break;
    case OpenFailure::Changed:
        text = u"its size changed since it was indexed"_s;
        break;
    case OpenFailure::NotOnDisk:
        text = u"its data is in iCloud only"_s;
        break;
    case OpenFailure::Unreadable:
        text = u"it can't be read"_s;
        break;
    }
    if (error != 0) {
        text += u": "_s + qt_error_string(error);
    }
    return text;
}

DescriptorSource::DescriptorSource(int fd, qint64 size, QString description)
    : m_fd(fd)
    , m_size(size)
    , m_description(std::move(description))
{}

DescriptorSource::~DescriptorSource()
{
    if (m_fd >= 0) {
        closeQuietly(m_fd);
    }
}

qint64 DescriptorSource::read(qint64 position, char *destination, qint64 maxBytes)
{
    if (position < 0 || maxBytes < 0 || (maxBytes > 0 && destination == nullptr)) {
        setLastError(audio::makeError(audio::ErrorCode::InvalidArgument,
                                      u"invalid read of %1 bytes at %2"_s.arg(maxBytes).arg(position)));
        return -1;
    }
    if (position >= m_size || maxBytes == 0) {
        return 0;
    }
    const qint64 count = std::min(maxBytes, m_size - position);
    qint64 done = 0;
    while (done < count) {
        const ssize_t got = ::pread(m_fd, destination + done, static_cast<size_t>(count - done), position + done);
        const int error = errno;
        if (got < 0 && error == EINTR) {
            continue;
        }
        if (got <= 0) {
            setLastError(audio::makeError(audio::ErrorCode::ReadFailed,
                                          got < 0 ? u"read failed at offset %1 of %2: %3"_s.arg(position + done)
                                                        .arg(m_description, qt_error_string(error))
                                                  : u"%1 is shorter than its size"_s.arg(m_description)));
            return -1;
        }
        done += got;
    }
    return done;
}

} // namespace selecta::share::detail
