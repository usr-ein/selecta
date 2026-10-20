// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/Error.h"

#include <QtGlobal>

#include <functional>
#include <memory>

struct AVIOContext;

namespace selecta::audio::detail {

// A read-only FFmpeg AVIOContext over a ByteSource (audio §5.4).
//
// - Non-seekable (AVIOContext::seekable = 0) while the source grows, so no demuxer seeks to the end of the file to
//   look for trailing tags or indexes; seekable once the source is complete (refreshSeekable()).
// - AVSEEK_SIZE reports the announced total size, so CBR durations and size checks work.
// - Reads never block and never return bytes at or beyond the available count. A read at that boundary while the
//   source grows sets the starvation flag and returns AVERROR_EOF, which FFmpeg doesn't keep as a sticky error; the
//   owner then discards whatever the demuxer produced and repositions once more bytes have arrived.
//
// Threading: used by the one thread running the demuxer.
class AvioAdapter
{
public:
    explicit AvioAdapter(std::shared_ptr<ByteSource> source);
    ~AvioAdapter();
    AvioAdapter(const AvioAdapter &) = delete;
    AvioAdapter &operator=(const AvioAdapter &) = delete;
    AvioAdapter(AvioAdapter &&) = delete;
    AvioAdapter &operator=(AvioAdapter &&) = delete;

    Result<void> create(int bufferSize = 64 * 1024);
    AVIOContext *context() const { return m_context; }

    // Makes the context seekable once the source is complete. Returns true when it is seekable.
    bool refreshSeekable();
    bool isSeekable() const;

    // True once a read hit the available boundary of a growing source; take() also clears it.
    bool starved() const { return m_starved; }
    bool takeStarved();
    // The available count when starvation happened.
    qint64 starvedAt() const { return m_starvedAt; }
    bool readFailed() const { return m_readFailed; }
    qint64 position() const { return m_position; }

    // Observers for tests: every read request (position, bytes asked, bytes returned or negative error) and every
    // seek request (offset, whence). Called on the demuxing thread.
    std::function<void(qint64, int, int)> readObserver;
    std::function<void(qint64, int)> seekObserver;

private:
    static int readThunk(void *opaque, quint8 *buffer, int size);
    static int64_t seekThunk(void *opaque, int64_t offset, int whence);
    int read(quint8 *buffer, int size);
    int64_t seek(int64_t offset, int whence);

    std::shared_ptr<ByteSource> m_source;
    AVIOContext *m_context = nullptr;
    qint64 m_position = 0;
    qint64 m_starvedAt = 0;
    bool m_starved = false;
    bool m_readFailed = false;
};

} // namespace selecta::audio::detail
