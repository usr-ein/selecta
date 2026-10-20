// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SyntheticAudio.h"
#include "audio/ByteSource.h"
#include "audio/Decoder.h"

#include <QByteArray>
#include <QMutex>

#include <atomic>
#include <memory>

namespace selecta::testing {

// A growing in-memory source that records every read request and counts the ones reaching past the available count
// of that instant: the contract's "no read goes past available" check, for every code path (own demuxers, FFmpeg's
// AVIOContext, index scans).
class LoggingSource final : public audio::ByteSource
{
public:
    explicit LoggingSource(QByteArray data, qint64 available = -1);

    void setAvailable(qint64 bytes);
    void setComplete();
    void setFailed();

    qint64 totalSize() const override { return m_data.size(); }
    audio::SourceSnapshot snapshot() const override;
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override;
    QString description() const override { return QStringLiteral("logging memory source"); }

    qint64 violations() const { return m_violations.load(); }
    qint64 reads() const { return m_reads.load(); }
    qint64 highestByteRead() const { return m_highest.load(); }

private:
    const QByteArray m_data;
    std::atomic<qint64> m_available{0};
    std::atomic<int> m_state{0};
    std::atomic<qint64> m_violations{0};
    std::atomic<qint64> m_reads{0};
    std::atomic<qint64> m_highest{0};
};

struct Decoded
{
    Pcm pcm;
    audio::DecodeStatus last = audio::DecodeStatus::Ok;
    audio::Decoder::OpenStatus open = audio::Decoder::OpenStatus::Failed;
    audio::Error error;
    audio::StreamInfo info;
    audio::DecoderStats stats;
    qint64 lengthFrames = -1;
};

// Decodes a whole source.
Decoded decodeAll(const std::shared_ptr<audio::ByteSource> &source, const audio::DecoderOptions &options = {});
Decoded decodeAll(const QByteArray &file, const audio::DecoderOptions &options = {});

// Decodes `file` while revealing it `step` bytes at a time, reading whatever is decodable after each step, as a preview
// would during a download. `source` (optional) receives the logging source used.
Decoded decodeProgressively(const QByteArray &file, qint64 step, const audio::DecoderOptions &options = {},
                            std::shared_ptr<LoggingSource> *source = nullptr);

// Reads up to `frames` frames from a decoder that is open (stops early at the end).
Pcm readFrames(audio::Decoder &decoder, qint64 frames);

} // namespace selecta::testing
