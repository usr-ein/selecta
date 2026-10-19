// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"

#include <QByteArray>
#include <QMutex>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

// Byte sources: where a Decoder reads a file from. A complete file on disk, a file that is still downloading (a
// preview), or bytes in memory.
namespace selecta::audio {

enum class SourceState {
    Growing,  // bytes are still arriving; available() grows
    Complete, // every byte is there; available() == totalSize()
    Failed,   // the download stopped for good (failed or cancelled); bytes below available() stay readable
};

// A consistent view of a source at one instant. Taken with ByteSource::snapshot(): when state is Complete,
// available is the final size.
struct SourceSnapshot
{
    qint64 available = 0;
    SourceState state = SourceState::Growing;

    bool isComplete() const { return state == SourceState::Complete; }
};

// RAII handle for a listener registered with ByteSource::subscribe(). Destroying it (or calling reset()) removes the
// listener; if the listener is running on another thread at that moment, the destructor waits for it to return, so
// whatever the listener captured may be destroyed right after. Movable, not copyable. Must not be destroyed from
// inside its own listener.
class Subscription
{
public:
    Subscription() = default;
    ~Subscription();
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
    Subscription(Subscription &&other) noexcept;
    Subscription &operator=(Subscription &&other) noexcept;

    void reset();
    bool isActive() const { return m_entry != nullptr; }

private:
    friend class ByteSource;
    struct Entry;
    explicit Subscription(std::shared_ptr<Entry> entry);

    std::shared_ptr<Entry> m_entry;
};

// Read-only access to the bytes of an audio file.
//
// Thread safety: every member is thread-safe. One source is typically shared (std::shared_ptr) by the writer (core's
// transfer, for a GrowingFile) and several readers (the player's Decoder, the analysis Decoder), each on its own
// thread.
//
// Reads never block and never return bytes at or beyond available(). Readers that need more bytes than are available
// stop and retry after a notification (subscribe()).
class ByteSource
{
public:
    ByteSource() = default;
    virtual ~ByteSource();
    ByteSource(const ByteSource &) = delete;
    ByteSource &operator=(const ByteSource &) = delete;
    ByteSource(ByteSource &&) = delete;
    ByteSource &operator=(ByteSource &&) = delete;

    // Final size in bytes: the file size of a complete file; for a preview, the size announced in the search result.
    // Never negative.
    virtual qint64 totalSize() const = 0;

    // State first, then the available byte count, read so that a Complete state always comes with the final count.
    virtual SourceSnapshot snapshot() const = 0;

    // Copies up to `maxBytes` bytes starting at `position` into `destination`. Never reads at or past the available
    // count of the moment. Returns the number of bytes copied: 0 when `position` is at or beyond available(), or when
    // `maxBytes` is 0; -1 on an I/O error (details in lastError()). Negative positions or sizes return -1.
    virtual qint64 read(qint64 position, char *destination, qint64 maxBytes) = 0;

    // For log messages: the file path, or "memory".
    virtual QString description() const = 0;

    // The last I/O error from read(), if any.
    Error lastError() const;

    // Convenience accessors over snapshot().
    qint64 available() const { return snapshot().available; }
    SourceState state() const { return snapshot().state; }

    // Calls `listener` (on the thread that changed the source) after every change of available() or state(). The
    // listener must be quick and thread-safe: typically it wakes a worker or posts an event. It is never called while
    // an internal lock is held, so it may call back into the source. Sources that never change (complete files) never
    // call it.
    [[nodiscard]] Subscription subscribe(std::function<void()> listener);

protected:
    // Subclasses call this after each change, without holding their own locks.
    void notifyListeners();
    void setLastError(const Error &error);

private:
    mutable QMutex m_listenersMutex;
    std::vector<std::shared_ptr<Subscription::Entry>> m_listeners;
    mutable QMutex m_errorMutex;
    Error m_lastError;
};

// A complete file on disk, opened read-only.
class FileSource final : public ByteSource
{
public:
    // Fails with OpenFailed when the path can't be opened for reading or isn't a regular file.
    static Result<std::shared_ptr<FileSource>> open(const QString &path);
    ~FileSource() override;
    FileSource(const FileSource &) = delete;
    FileSource &operator=(const FileSource &) = delete;
    FileSource(FileSource &&) = delete;
    FileSource &operator=(FileSource &&) = delete;

    qint64 totalSize() const override { return m_size; }
    SourceSnapshot snapshot() const override { return {m_size, SourceState::Complete}; }
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override;
    QString description() const override { return m_path; }

private:
    struct Handle;
    FileSource(QString path, std::unique_ptr<Handle> handle, qint64 size);

    QString m_path;
    std::unique_ptr<Handle> m_handle;
    qint64 m_size = 0;
};

// A file that is still being written by a download: the preview cache file of PREV-1.
//
// The writer (core's transfer) writes the bytes to the file itself, in order from offset 0, then publishes them with
// setAvailable(); only bytes below that count are ever read, so a reader never sees a partly written block. The
// writer may run on any thread; readers open nothing themselves and share this object's read-only handle.
//
// The file is opened read-only at construction; it may be empty or shorter than totalSize at that point.
class GrowingFile final : public ByteSource
{
public:
    // `totalSize` is the announced final size (> 0). Fails with OpenFailed if the file can't be opened for reading,
    // InvalidArgument for a non-positive size.
    static Result<std::shared_ptr<GrowingFile>> open(const QString &path, qint64 totalSize);
    ~GrowingFile() override;
    GrowingFile(const GrowingFile &) = delete;
    GrowingFile &operator=(const GrowingFile &) = delete;
    GrowingFile(GrowingFile &&) = delete;
    GrowingFile &operator=(GrowingFile &&) = delete;

    // Writer side. Call after the bytes [0, bytes) have been written (write() returned). The count never decreases:
    // a smaller value is ignored; a value above totalSize is clamped. Ignored after setComplete() or setFailed().
    void setAvailable(qint64 bytes);
    // Every byte has arrived: available becomes totalSize. A download that ended early is a failure, not completion.
    void setComplete();
    // The download stopped for good. Bytes already published stay readable; decoders that need more report
    // SourceFailed. Ignored after setComplete().
    void setFailed();

    qint64 totalSize() const override { return m_totalSize; }
    SourceSnapshot snapshot() const override;
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override;
    QString description() const override { return m_path; }

private:
    struct Handle;
    GrowingFile(QString path, std::unique_ptr<Handle> handle, qint64 totalSize);

    QString m_path;
    std::unique_ptr<Handle> m_handle;
    qint64 m_totalSize = 0;
    std::atomic<qint64> m_available{0};
    std::atomic<int> m_state{static_cast<int>(SourceState::Growing)};
};

// Bytes in memory. Starts complete, unless constructed with a smaller available count, in which case it behaves like
// a GrowingFile (setAvailable(), setComplete(), setFailed()): tests use this to simulate a download.
class MemorySource final : public ByteSource
{
public:
    explicit MemorySource(QByteArray data, qint64 available = -1);
    ~MemorySource() override;
    MemorySource(const MemorySource &) = delete;
    MemorySource &operator=(const MemorySource &) = delete;
    MemorySource(MemorySource &&) = delete;
    MemorySource &operator=(MemorySource &&) = delete;

    void setAvailable(qint64 bytes);
    void setComplete();
    void setFailed();

    qint64 totalSize() const override { return m_data.size(); }
    SourceSnapshot snapshot() const override;
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override;
    QString description() const override { return QStringLiteral("memory"); }

private:
    const QByteArray m_data;
    std::atomic<qint64> m_available{0};
    std::atomic<int> m_state{static_cast<int>(SourceState::Complete)};
};

} // namespace selecta::audio
