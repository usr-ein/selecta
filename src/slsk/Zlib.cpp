// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Zlib.h"

#include <algorithm>
#include <limits>
// next_in is then `const Bytef *`: zlib never writes through it.
#define ZLIB_CONST
#include <zlib.h>

namespace selecta::slsk::zlib {
namespace {

constexpr qsizetype kInitialOutput = qsizetype{64} * 1024;
constexpr qsizetype kMaxChunk = std::numeric_limits<uInt>::max();

// RAII for an inflate stream.
class InflateStream
{
public:
    InflateStream()
        : m_ok(inflateInit(&m_stream) == Z_OK)
    {}
    ~InflateStream()
    {
        if (m_ok) {
            inflateEnd(&m_stream);
        }
    }
    InflateStream(const InflateStream &) = delete;
    InflateStream &operator=(const InflateStream &) = delete;

    bool ok() const { return m_ok; }
    z_stream *operator->() { return &m_stream; }
    z_stream *get() { return &m_stream; }

private:
    z_stream m_stream{};
    bool m_ok;
};

// Feeds the next slice of `input` (zlib's counters are 32-bit) once the previous one is used up.
void refill(z_stream *stream, QByteArrayView input, qsizetype &consumed)
{
    if (stream->avail_in == 0 && consumed < input.size()) {
        const qsizetype chunk = std::min(input.size() - consumed, kMaxChunk);
        stream->next_in = reinterpret_cast<const Bytef *>(input.data() + consumed);
        stream->avail_in = static_cast<uInt>(chunk);
        consumed += chunk;
    }
}

// Inflates into `out` until the stream ends or `limit` bytes are produced. `limit` may exceed the caller's cap by one
// byte so that crossing the cap is detectable.
Error run(QByteArrayView input, qsizetype limit, QByteArray &out, bool stopAtLimit, bool *ended)
{
    *ended = false;
    InflateStream stream;
    if (!stream.ok()) {
        return Error::Corrupt;
    }
    qsizetype consumed = 0;
    qsizetype produced = 0;
    out.clear();
    for (;;) {
        refill(stream.get(), input, consumed);
        if (produced == out.size()) {
            if (produced >= limit) {
                return Error::None; // reached the limit; the caller decides what that means
            }
            const qsizetype grown = std::max(out.size() * 2, std::min(limit, kInitialOutput));
            out.resize(std::min(grown, limit));
        }
        stream->next_out = reinterpret_cast<Bytef *>(out.data() + produced);
        const qsizetype room = std::min(out.size() - produced, kMaxChunk);
        stream->avail_out = static_cast<uInt>(room);
        const uInt inBefore = stream->avail_in;
        const int result = ::inflate(stream.get(), Z_NO_FLUSH);
        produced += room - static_cast<qsizetype>(stream->avail_out);
        if (result == Z_STREAM_END) {
            out.resize(produced);
            *ended = true;
            // Trailing bytes: anything left in this slice, or slices not fed yet.
            return (stream->avail_in > 0 || consumed < input.size()) ? Error::TrailingData : Error::None;
        }
        if (stopAtLimit && produced >= limit) {
            out.resize(produced);
            return Error::None;
        }
        if (result == Z_OK) {
            continue;
        }
        if (result == Z_BUF_ERROR) {
            // No progress possible: either we're out of input (truncated) or out of output room (loop grows it).
            const bool noInputLeft = stream->avail_in == 0 && consumed >= input.size();
            if (noInputLeft || (inBefore == stream->avail_in && stream->avail_out > 0)) {
                out.resize(produced);
                return Error::Truncated;
            }
            continue;
        }
        out.resize(produced);
        return Error::Corrupt; // Z_DATA_ERROR, Z_NEED_DICT, Z_MEM_ERROR, Z_STREAM_ERROR
    }
}

} // namespace

const char *errorName(Error error)
{
    switch (error) {
    case Error::None:
        return "none";
    case Error::Truncated:
        return "truncated";
    case Error::TrailingData:
        return "trailing data";
    case Error::TooLarge:
        return "too large";
    case Error::Corrupt:
        return "corrupt";
    }
    return "?";
}

Inflated inflate(QByteArrayView compressed, qsizetype maxOutput)
{
    Inflated result;
    if (maxOutput < 0) {
        result.error = Error::TooLarge;
        return result;
    }
    if (compressed.isEmpty()) {
        result.error = Error::Truncated;
        return result;
    }
    // One byte of headroom: producing it means the cap was crossed.
    const qsizetype limit = maxOutput < std::numeric_limits<qsizetype>::max() ? maxOutput + 1 : maxOutput;
    bool ended = false;
    QByteArray out;
    const Error error = run(compressed, limit, out, false, &ended);
    if (out.size() > maxOutput) {
        result.error = Error::TooLarge;
        return result;
    }
    if (error != Error::None) {
        result.error = error;
        return result;
    }
    if (!ended) {
        // Stopped at the limit without the stream ending: it has more to give than the cap allows.
        result.error = Error::TooLarge;
        return result;
    }
    result.data = std::move(out);
    return result;
}

Inflated inflatePrefix(QByteArrayView compressed, qsizetype prefixBytes)
{
    Inflated result;
    if (compressed.isEmpty() || prefixBytes < 0) {
        result.error = Error::Truncated;
        return result;
    }
    bool ended = false;
    QByteArray out;
    const Error error = run(compressed, prefixBytes, out, true, &ended);
    // Only the prefix matters: trailing data after a short stream is not this function's concern, and a truncated
    // stream that still yielded the whole prefix is fine.
    const bool prefixComplete = error == Error::Truncated && out.size() >= prefixBytes;
    if (error != Error::None && error != Error::TrailingData && !prefixComplete) {
        result.error = error;
        return result;
    }
    out.truncate(prefixBytes);
    result.data = std::move(out);
    return result;
}

QByteArray deflate(QByteArrayView raw, int level)
{
    z_stream stream{};
    if (deflateInit(&stream, std::clamp(level, 0, 9)) != Z_OK) {
        return {};
    }
    QByteArray out;
    qsizetype consumed = 0;
    qsizetype produced = 0;
    int result = Z_OK;
    do {
        refill(&stream, raw, consumed);
        const int flush = consumed >= raw.size() ? Z_FINISH : Z_NO_FLUSH;
        if (produced == out.size()) {
            out.resize(std::max<qsizetype>(out.size() * 2, 1024));
        }
        const qsizetype room = std::min(out.size() - produced, kMaxChunk);
        stream.next_out = reinterpret_cast<Bytef *>(out.data() + produced);
        stream.avail_out = static_cast<uInt>(room);
        result = ::deflate(&stream, flush);
        produced += room - static_cast<qsizetype>(stream.avail_out);
    } while (result == Z_OK || result == Z_BUF_ERROR);
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        return {};
    }
    out.resize(produced);
    return out;
}

} // namespace selecta::slsk::zlib
