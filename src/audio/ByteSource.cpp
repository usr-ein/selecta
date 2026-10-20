// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/ByteSource.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRecursiveMutex>

#include <algorithm>
#include <cstring>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::audio {

// ---------------------------------------------------------------------------------------------------------------
// Subscription

struct Subscription::Entry
{
    explicit Entry(std::function<void()> callback)
        : listener(std::move(callback))
    {}

    std::function<void()> listener;
    // Held while the listener runs, so reset() can wait for an in-flight call. Recursive: a listener may drop its own
    // subscription.
    QRecursiveMutex callMutex;
    bool active = true;                 // guarded by callMutex
    std::atomic<bool> pruneHint{false}; // set once inactive, read without the call mutex when pruning the list
};

Subscription::Subscription(std::shared_ptr<Entry> entry)
    : m_entry(std::move(entry))
{}

Subscription::~Subscription()
{
    reset();
}

Subscription::Subscription(Subscription &&other) noexcept
    : m_entry(std::move(other.m_entry))
{}

Subscription &Subscription::operator=(Subscription &&other) noexcept
{
    if (this != &other) {
        reset();
        m_entry = std::move(other.m_entry);
    }
    return *this;
}

void Subscription::reset()
{
    if (!m_entry) {
        return;
    }
    {
        const QMutexLocker lock(&m_entry->callMutex);
        m_entry->active = false;
        m_entry->pruneHint.store(true, std::memory_order_release);
    }
    m_entry.reset();
}

// ---------------------------------------------------------------------------------------------------------------
// ByteSource

ByteSource::~ByteSource() = default;

Error ByteSource::lastError() const
{
    const QMutexLocker lock(&m_errorMutex);
    return m_lastError;
}

void ByteSource::setLastError(const Error &error)
{
    const QMutexLocker lock(&m_errorMutex);
    m_lastError = error;
}

Subscription ByteSource::subscribe(std::function<void()> listener)
{
    auto entry = std::make_shared<Subscription::Entry>(std::move(listener));
    {
        const QMutexLocker lock(&m_listenersMutex);
        std::erase_if(m_listeners, [](const std::shared_ptr<Subscription::Entry> &e) {
            return e->pruneHint.load(std::memory_order_acquire);
        });
        m_listeners.push_back(entry);
    }
    return Subscription(std::move(entry));
}

void ByteSource::notifyListeners()
{
    std::vector<std::shared_ptr<Subscription::Entry>> listeners;
    {
        const QMutexLocker lock(&m_listenersMutex);
        std::erase_if(m_listeners, [](const std::shared_ptr<Subscription::Entry> &e) {
            return e->pruneHint.load(std::memory_order_acquire);
        });
        listeners = m_listeners;
    }
    for (const std::shared_ptr<Subscription::Entry> &entry : listeners) {
        const QMutexLocker lock(&entry->callMutex);
        if (entry->active && entry->listener) {
            entry->listener();
        }
    }
}

namespace {

// Reads exactly `count` bytes at `position` from a file handle, or fails. Shared by FileSource and GrowingFile.
#ifdef Q_OS_UNIX
struct PosixFile
{
    int fd = -1;

    PosixFile() = default;
    ~PosixFile()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    PosixFile(const PosixFile &) = delete;
    PosixFile &operator=(const PosixFile &) = delete;
    PosixFile(PosixFile &&) = delete;
    PosixFile &operator=(PosixFile &&) = delete;

    bool open(const QString &path, QString *why)
    {
        const QByteArray native = QFile::encodeName(path);
        do {
            fd = ::open(native.constData(), O_RDONLY | O_CLOEXEC); // NOLINT(cppcoreguidelines-pro-type-vararg)
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            *why = QString::fromLocal8Bit(std::strerror(errno)); // NOLINT(concurrency-mt-unsafe): message only
            return false;
        }
        struct stat info = {};
        if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            *why = u"not a regular file"_s;
            return false;
        }
        return true;
    }

    qint64 size() const
    {
        struct stat info = {};
        return ::fstat(fd, &info) == 0 ? static_cast<qint64>(info.st_size) : -1;
    }

    // Returns the bytes read; fewer than `count` only at the end of the file. -1 on error.
    qint64 readAt(qint64 position, char *destination, qint64 count) const
    {
        qint64 done = 0;
        while (done < count) {
            const ssize_t n =
                ::pread(fd, destination + done, static_cast<size_t>(count - done), static_cast<off_t>(position + done));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                break;
            }
            done += n;
        }
        return done;
    }
};
using NativeFile = PosixFile;
#else
struct QtFile
{
    QFile file;
    mutable QMutex mutex;

    bool open(const QString &path, QString *why)
    {
        file.setFileName(path);
        if (!QFileInfo(path).isFile()) {
            *why = u"not a regular file"_s;
            return false;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            *why = file.errorString();
            return false;
        }
        return true;
    }
    qint64 size() const { return file.size(); }
    qint64 readAt(qint64 position, char *destination, qint64 count) const
    {
        const QMutexLocker lock(&mutex);
        auto &f = const_cast<QFile &>(file); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (!f.seek(position)) {
            return -1;
        }
        qint64 done = 0;
        while (done < count) {
            const qint64 n = f.read(destination + done, count - done);
            if (n < 0) {
                return -1;
            }
            if (n == 0) {
                break;
            }
            done += n;
        }
        return done;
    }
};
using NativeFile = QtFile;
#endif

// Common read logic: bounds against the available count, exact reads, errors recorded.
qint64 boundedRead(const NativeFile &file, qint64 available, qint64 position, char *destination, qint64 maxBytes,
                   const QString &path, Error *error)
{
    if (position < 0 || maxBytes < 0 || (maxBytes > 0 && destination == nullptr)) {
        *error = makeError(ErrorCode::InvalidArgument, u"invalid read of %1 bytes at %2"_s.arg(maxBytes).arg(position));
        return -1;
    }
    if (position >= available || maxBytes == 0) {
        return 0;
    }
    const qint64 count = std::min(maxBytes, available - position);
    const qint64 got = file.readAt(position, destination, count);
    if (got < 0) {
        *error = makeError(ErrorCode::ReadFailed, u"read failed at offset %1 of %2"_s.arg(position).arg(path));
        return -1;
    }
    if (got < count) {
        // The bytes were published as written but aren't there: the file was truncated behind our back.
        *error = makeError(ErrorCode::ReadFailed,
                           u"%1 is shorter than its published size (%2 bytes at offset %3, expected %4)"_s.arg(path)
                               .arg(got)
                               .arg(position)
                               .arg(count));
        return -1;
    }
    return got;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// FileSource

struct FileSource::Handle
{
    NativeFile file;
};

FileSource::FileSource(QString path, std::unique_ptr<Handle> handle, qint64 size)
    : m_path(std::move(path))
    , m_handle(std::move(handle))
    , m_size(size)
{}

FileSource::~FileSource() = default;

Result<std::shared_ptr<FileSource>> FileSource::open(const QString &path)
{
    auto handle = std::make_unique<Handle>();
    QString why;
    if (!handle->file.open(path, &why)) {
        return makeError(ErrorCode::OpenFailed, u"can't open %1: %2"_s.arg(path, why));
    }
    const qint64 size = handle->file.size();
    if (size < 0) {
        return makeError(ErrorCode::OpenFailed, u"can't read the size of %1"_s.arg(path));
    }
    return std::shared_ptr<FileSource>(new FileSource(path, std::move(handle), size));
}

qint64 FileSource::read(qint64 position, char *destination, qint64 maxBytes)
{
    Error error;
    const qint64 got = boundedRead(m_handle->file, m_size, position, destination, maxBytes, m_path, &error);
    if (got < 0) {
        setLastError(error);
    }
    return got;
}

// ---------------------------------------------------------------------------------------------------------------
// GrowingFile

struct GrowingFile::Handle
{
    NativeFile file;
};

GrowingFile::GrowingFile(QString path, std::unique_ptr<Handle> handle, qint64 totalSize)
    : m_path(std::move(path))
    , m_handle(std::move(handle))
    , m_totalSize(totalSize)
{}

GrowingFile::~GrowingFile() = default;

Result<std::shared_ptr<GrowingFile>> GrowingFile::open(const QString &path, qint64 totalSize)
{
    if (totalSize <= 0) {
        return makeError(ErrorCode::InvalidArgument, u"announced size must be positive, got %1"_s.arg(totalSize));
    }
    auto handle = std::make_unique<Handle>();
    QString why;
    if (!handle->file.open(path, &why)) {
        return makeError(ErrorCode::OpenFailed, u"can't open %1: %2"_s.arg(path, why));
    }
    return std::shared_ptr<GrowingFile>(new GrowingFile(path, std::move(handle), totalSize));
}

void GrowingFile::setAvailable(qint64 bytes)
{
    if (m_state.load(std::memory_order_acquire) != static_cast<int>(SourceState::Growing)) {
        return;
    }
    const qint64 target = std::clamp<qint64>(bytes, 0, m_totalSize);
    qint64 current = m_available.load(std::memory_order_relaxed);
    bool grew = false;
    while (target > current) {
        if (m_available.compare_exchange_weak(current, target, std::memory_order_release, std::memory_order_relaxed)) {
            grew = true;
            break;
        }
    }
    if (grew) {
        notifyListeners();
    }
}

void GrowingFile::setComplete()
{
    int expected = static_cast<int>(SourceState::Growing);
    m_available.store(m_totalSize, std::memory_order_release);
    if (m_state.compare_exchange_strong(expected, static_cast<int>(SourceState::Complete), std::memory_order_acq_rel)) {
        notifyListeners();
    }
}

void GrowingFile::setFailed()
{
    int expected = static_cast<int>(SourceState::Growing);
    if (m_state.compare_exchange_strong(expected, static_cast<int>(SourceState::Failed), std::memory_order_acq_rel)) {
        notifyListeners();
    }
}

SourceSnapshot GrowingFile::snapshot() const
{
    const auto state = static_cast<SourceState>(m_state.load(std::memory_order_acquire));
    const qint64 available = m_available.load(std::memory_order_acquire);
    return {state == SourceState::Complete ? m_totalSize : available, state};
}

qint64 GrowingFile::read(qint64 position, char *destination, qint64 maxBytes)
{
    Error error;
    const qint64 got =
        boundedRead(m_handle->file, snapshot().available, position, destination, maxBytes, m_path, &error);
    if (got < 0) {
        setLastError(error);
    }
    return got;
}

// ---------------------------------------------------------------------------------------------------------------
// MemorySource

MemorySource::MemorySource(QByteArray data, qint64 available)
    : m_data(std::move(data))
{
    if (available < 0) {
        m_available.store(m_data.size(), std::memory_order_relaxed);
        m_state.store(static_cast<int>(SourceState::Complete), std::memory_order_relaxed);
    } else {
        m_available.store(std::min<qint64>(available, m_data.size()), std::memory_order_relaxed);
        m_state.store(static_cast<int>(SourceState::Growing), std::memory_order_relaxed);
    }
}

MemorySource::~MemorySource() = default;

void MemorySource::setAvailable(qint64 bytes)
{
    if (m_state.load(std::memory_order_acquire) != static_cast<int>(SourceState::Growing)) {
        return;
    }
    const qint64 target = std::clamp<qint64>(bytes, 0, m_data.size());
    qint64 current = m_available.load(std::memory_order_relaxed);
    bool grew = false;
    while (target > current) {
        if (m_available.compare_exchange_weak(current, target, std::memory_order_release, std::memory_order_relaxed)) {
            grew = true;
            break;
        }
    }
    if (grew) {
        notifyListeners();
    }
}

void MemorySource::setComplete()
{
    int expected = static_cast<int>(SourceState::Growing);
    m_available.store(m_data.size(), std::memory_order_release);
    if (m_state.compare_exchange_strong(expected, static_cast<int>(SourceState::Complete), std::memory_order_acq_rel)) {
        notifyListeners();
    }
}

void MemorySource::setFailed()
{
    int expected = static_cast<int>(SourceState::Growing);
    if (m_state.compare_exchange_strong(expected, static_cast<int>(SourceState::Failed), std::memory_order_acq_rel)) {
        notifyListeners();
    }
}

SourceSnapshot MemorySource::snapshot() const
{
    const auto state = static_cast<SourceState>(m_state.load(std::memory_order_acquire));
    const qint64 available = m_available.load(std::memory_order_acquire);
    return {state == SourceState::Complete ? m_data.size() : available, state};
}

qint64 MemorySource::read(qint64 position, char *destination, qint64 maxBytes)
{
    if (position < 0 || maxBytes < 0 || (maxBytes > 0 && destination == nullptr)) {
        setLastError(
            makeError(ErrorCode::InvalidArgument, u"invalid read of %1 bytes at %2"_s.arg(maxBytes).arg(position)));
        return -1;
    }
    const qint64 available = snapshot().available;
    if (position >= available || maxBytes == 0) {
        return 0;
    }
    const qint64 count = std::min(maxBytes, available - position);
    std::memcpy(destination, m_data.constData() + position, static_cast<size_t>(count));
    return count;
}

} // namespace selecta::audio
