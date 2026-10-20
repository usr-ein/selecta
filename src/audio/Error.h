// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLoggingCategory>
#include <QMetaType>
#include <QString>

#include <atomic>
#include <utility>
#include <variant>

Q_DECLARE_LOGGING_CATEGORY(lcAudio)

// Error model shared by the media libraries (audio/ and convert/). Nothing in them throws: every operation that can
// fail returns a Result<T> or a status plus an Error. Messages are for logs and tooltips, in English, and never contain
// file contents.
namespace selecta::audio {

enum class ErrorCode {
    None,
    OpenFailed,         // the file can't be opened (missing, permissions, not a regular file)
    ReadFailed,         // an I/O error while reading
    WriteFailed,        // an I/O error while writing (cache, encoder output, tags)
    DiskFull,           // a write failed for lack of space
    OutputExists,       // the output path already exists; media code never overwrites
    Unsupported,        // a format, codec or option this build doesn't handle
    NeedsCompleteFile,  // the format can't be decoded while it downloads (e.g. M4A with `moov` at the end)
    SourceFailed,       // the download feeding a growing source failed or was cancelled
    Corrupt,            // malformed or hostile data
    TooLarge,           // a safety bound was exceeded (tag, picture, frame, duration, image size)
    DecoderError,       // FFmpeg or the decoder reported an error
    EncoderError,       // LAME, FFmpeg or soxr reported an error while encoding
    TagError,           // TagLib couldn't read or write the tags
    VerificationFailed, // the output didn't pass OutputVerifier
    Cancelled,          // the caller cancelled the operation
    InvalidArgument,    // the caller passed an invalid option or state
    Internal,           // an unexpected failure (allocation, library misuse); a bug if it ever shows up
};

// Stable English name of a code, e.g. "NeedsCompleteFile", for logs and tests.
QString errorCodeName(ErrorCode code);

struct Error
{
    ErrorCode code = ErrorCode::None;
    QString message; // what failed and why, e.g. "ID3v2 tag of 40 MB exceeds the 16 MB limit"

    bool isError() const { return code != ErrorCode::None; }
    // "Corrupt: ID3v2 tag size field is invalid"
    QString toString() const;

    friend bool operator==(const Error &a, const Error &b) = default;
};

inline Error makeError(ErrorCode code, QString message)
{
    return Error{code, std::move(message)};
}

// A value or an Error, like C++23 std::expected. Copyable and movable when T is. Accessing value() on an error (or
// error() on a value) is a programming error: it asserts in debug builds; in release builds value() returns a
// default-constructed T held for that purpose and error() an Internal error.
template <typename T>
class Result
{
public:
    Result(T value) // NOLINT(cppcoreguidelines-explicit-constructor,misc-explicit-constructor): like std::expected
        : m_data(std::in_place_index<0>, std::move(value))
    {}
    Result(Error error) // NOLINT(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
        : m_data(std::in_place_index<1>, std::move(error))
    {}

    bool ok() const { return m_data.index() == 0; }
    explicit operator bool() const { return ok(); }

    const T &value() const &
    {
        Q_ASSERT(ok());
        return ok() ? std::get<0>(m_data) : fallbackValue();
    }
    T &value() &
    {
        Q_ASSERT(ok());
        return ok() ? std::get<0>(m_data) : fallbackValue();
    }
    // Moves the value out; the Result is left holding a moved-from T.
    T takeValue()
    {
        Q_ASSERT(ok());
        return ok() ? std::move(std::get<0>(m_data)) : T{};
    }
    const Error &error() const
    {
        Q_ASSERT(!ok());
        return ok() ? internalError() : std::get<1>(m_data);
    }

private:
    static T &fallbackValue()
    {
        thread_local T fallback{};
        fallback = T{};
        return fallback;
    }
    static const Error &internalError()
    {
        static const Error error{ErrorCode::Internal, QStringLiteral("Result accessed in the wrong state")};
        return error;
    }

    std::variant<T, Error> m_data;
};

// Result<void>: success or an Error.
template <>
class Result<void>
{
public:
    Result() = default;
    Result(Error error) // NOLINT(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
        : m_error(std::move(error))
    {}

    bool ok() const { return !m_error.isError(); }
    explicit operator bool() const { return ok(); }
    const Error &error() const { return m_error; }

private:
    Error m_error;
};

// A cancellation request shared between the caller and a worker. Thread-safe: cancel() from any thread; workers poll
// isCancelled() between units of work (a packet, a chunk, a buffer). Cancellation is never undone.
class CancellationFlag
{
public:
    void cancel() { m_cancelled.store(true, std::memory_order_release); }
    bool isCancelled() const { return m_cancelled.load(std::memory_order_acquire); }

private:
    std::atomic<bool> m_cancelled{false};
};

} // namespace selecta::audio

Q_DECLARE_METATYPE(selecta::audio::Error)
