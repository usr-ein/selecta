// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/AvioAdapter.h"

#include "audio/detail/Ffmpeg.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/mem.h>
}

#include <cstdio>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {

AvioAdapter::AvioAdapter(std::shared_ptr<ByteSource> source)
    : m_source(std::move(source))
{}

AvioAdapter::~AvioAdapter()
{
    if (m_context != nullptr) {
        av_freep(static_cast<void *>(&m_context->buffer));
        avio_context_free(&m_context);
    }
}

Result<void> AvioAdapter::create(int bufferSize)
{
    auto *buffer = static_cast<unsigned char *>(av_malloc(static_cast<size_t>(bufferSize)));
    if (buffer == nullptr) {
        return makeError(ErrorCode::Internal, u"out of memory allocating an I/O buffer"_s);
    }
    m_context =
        avio_alloc_context(buffer, bufferSize, 0, this, &AvioAdapter::readThunk, nullptr, &AvioAdapter::seekThunk);
    if (m_context == nullptr) {
        av_free(buffer);
        return makeError(ErrorCode::Internal, u"out of memory allocating an I/O context"_s);
    }
    m_context->seekable = 0;
    refreshSeekable();
    return {};
}

bool AvioAdapter::refreshSeekable()
{
    if (m_context == nullptr) {
        return false;
    }
    if (m_source->snapshot().isComplete()) {
        m_context->seekable = AVIO_SEEKABLE_NORMAL;
    }
    return isSeekable();
}

bool AvioAdapter::isSeekable() const
{
    return m_context != nullptr && hasFlag(m_context->seekable, AVIO_SEEKABLE_NORMAL);
}

bool AvioAdapter::takeStarved()
{
    const bool starved = m_starved;
    m_starved = false;
    return starved;
}

int AvioAdapter::readThunk(void *opaque, quint8 *buffer, int size)
{
    return static_cast<AvioAdapter *>(opaque)->read(buffer, size);
}

int64_t AvioAdapter::seekThunk(void *opaque, int64_t offset, int whence)
{
    return static_cast<AvioAdapter *>(opaque)->seek(offset, whence);
}

int AvioAdapter::read(quint8 *buffer, int size)
{
    const SourceSnapshot snapshot = m_source->snapshot();
    int result = 0;
    if (size <= 0) {
        result = 0;
    } else if (m_position >= snapshot.available) {
        if (!snapshot.isComplete()) {
            m_starved = true;
            m_starvedAt = snapshot.available;
        }
        result = ffErrorEof();
    } else {
        const qint64 count = std::min<qint64>(size, snapshot.available - m_position);
        const qint64 got = m_source->read(m_position, reinterpret_cast<char *>(buffer), count);
        if (got < 0) {
            m_readFailed = true;
            result = ffErrorEio();
        } else if (got == 0) {
            result = ffErrorEof();
        } else {
            m_position += got;
            result = static_cast<int>(got);
        }
    }
    if (readObserver) {
        readObserver(m_position - (result > 0 ? result : 0), size, result);
    }
    return result;
}

int64_t AvioAdapter::seek(int64_t offset, int whence)
{
    if (seekObserver) {
        seekObserver(offset, whence);
    }
    if (hasFlag(whence, AVSEEK_SIZE)) {
        return m_source->totalSize();
    }
    const int mode = withoutFlag(whence, AVSEEK_FORCE);
    qint64 target = 0;
    switch (mode) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = m_position + offset;
        break;
    case SEEK_END:
        target = m_source->totalSize() + offset;
        break;
    default:
        return ffErrorEio();
    }
    if (target < 0) {
        return ffErrorEio();
    }
    m_position = target;
    return target;
}

} // namespace selecta::audio::detail
