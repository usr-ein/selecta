// SPDX-License-Identifier: GPL-3.0-or-later
#include "DecodeHelpers.h"

#include <algorithm>
#include <cstring>

namespace selecta::testing {

using audio::Decoder;
using audio::DecodeStatus;
using audio::SourceState;

LoggingSource::LoggingSource(QByteArray data, qint64 available)
    : m_data(std::move(data))
{
    if (available < 0) {
        m_available = m_data.size();
        m_state = static_cast<int>(SourceState::Complete);
    } else {
        m_available = std::min<qint64>(available, m_data.size());
        m_state = static_cast<int>(SourceState::Growing);
    }
}

void LoggingSource::setAvailable(qint64 bytes)
{
    if (m_state.load() != static_cast<int>(SourceState::Growing)) {
        return;
    }
    const qint64 target = std::clamp<qint64>(bytes, 0, m_data.size());
    if (target > m_available.load()) {
        m_available.store(target);
        notifyListeners();
    }
}

void LoggingSource::setComplete()
{
    m_available.store(m_data.size());
    m_state.store(static_cast<int>(SourceState::Complete));
    notifyListeners();
}

void LoggingSource::setFailed()
{
    m_state.store(static_cast<int>(SourceState::Failed));
    notifyListeners();
}

audio::SourceSnapshot LoggingSource::snapshot() const
{
    const auto state = static_cast<SourceState>(m_state.load());
    return {state == SourceState::Complete ? m_data.size() : m_available.load(), state};
}

qint64 LoggingSource::read(qint64 position, char *destination, qint64 maxBytes)
{
    ++m_reads;
    const qint64 available = snapshot().available;
    if (position < 0 || maxBytes < 0 || position + maxBytes > available) {
        ++m_violations;
    }
    if (position < 0 || maxBytes < 0) {
        return -1;
    }
    if (position >= available || maxBytes == 0) {
        return 0;
    }
    const qint64 count = std::min(maxBytes, available - position);
    std::memcpy(destination, m_data.constData() + position, static_cast<std::size_t>(count));
    qint64 highest = m_highest.load();
    while (position + count > highest && !m_highest.compare_exchange_weak(highest, position + count)) {
    }
    return count;
}

Pcm readFrames(Decoder &decoder, qint64 frames)
{
    Pcm pcm;
    pcm.sampleRate = decoder.sampleRate();
    pcm.channels = decoder.channels();
    std::vector<float> buffer(static_cast<std::size_t>(4096 * pcm.channels));
    while (pcm.frames() < frames) {
        const qint64 want = std::min<qint64>(4096, frames - pcm.frames());
        const audio::ReadResult r = decoder.read(buffer.data(), want);
        pcm.samples.insert(pcm.samples.end(), buffer.begin(),
                           buffer.begin() + static_cast<std::ptrdiff_t>(r.frames * pcm.channels));
        if (r.status != DecodeStatus::Ok) {
            break;
        }
    }
    return pcm;
}

Decoded decodeAll(const std::shared_ptr<audio::ByteSource> &source, const audio::DecoderOptions &options)
{
    Decoded out;
    Decoder decoder;
    out.open = decoder.open(source, options);
    if (out.open != Decoder::OpenStatus::Ready) {
        out.error = decoder.error();
        out.last = DecodeStatus::Error;
        return out;
    }
    out.info = decoder.info();
    out.lengthFrames = decoder.lengthFrames();
    out.pcm.sampleRate = decoder.sampleRate();
    out.pcm.channels = decoder.channels();
    std::vector<float> buffer(static_cast<std::size_t>(4096 * decoder.channels()));
    for (int guard = 0; guard < 10'000'000; ++guard) {
        const audio::ReadResult r = decoder.read(buffer.data(), 4096);
        out.pcm.samples.insert(out.pcm.samples.end(), buffer.begin(),
                               buffer.begin() + static_cast<std::ptrdiff_t>(r.frames * decoder.channels()));
        out.last = r.status;
        if (r.status == DecodeStatus::EndOfStream || r.status == DecodeStatus::Error ||
            r.status == DecodeStatus::NeedMoreData) {
            break;
        }
    }
    out.error = decoder.error();
    out.info = decoder.info();
    out.stats = decoder.stats();
    out.lengthFrames = decoder.lengthFrames();
    return out;
}

Decoded decodeAll(const QByteArray &file, const audio::DecoderOptions &options)
{
    return decodeAll(std::make_shared<audio::MemorySource>(file), options);
}

Decoded decodeProgressively(const QByteArray &file, qint64 step, const audio::DecoderOptions &options,
                            std::shared_ptr<LoggingSource> *sourceOut)
{
    auto source = std::make_shared<LoggingSource>(file, 0);
    if (sourceOut != nullptr) {
        *sourceOut = source;
    }
    qint64 available = 0;
    const auto grow = [&] {
        available = std::min<qint64>(file.size(), available + step);
        source->setAvailable(available);
        if (available >= file.size()) {
            source->setComplete();
        }
    };
    Decoded out;
    Decoder decoder;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        out.open = decoder.open(source, options);
        if (out.open == Decoder::OpenStatus::Ready || out.open == Decoder::OpenStatus::Failed) {
            break;
        }
        if (source->snapshot().isComplete()) {
            break;
        }
        grow();
    }
    if (out.open != Decoder::OpenStatus::Ready) {
        out.error = decoder.error();
        out.last = DecodeStatus::Error;
        return out;
    }
    out.pcm.sampleRate = decoder.sampleRate();
    out.pcm.channels = decoder.channels();
    std::vector<float> buffer(static_cast<std::size_t>(4096 * decoder.channels()));
    for (int guard = 0; guard < 10'000'000; ++guard) {
        const audio::ReadResult r = decoder.read(buffer.data(), 4096);
        out.pcm.samples.insert(out.pcm.samples.end(), buffer.begin(),
                               buffer.begin() + static_cast<std::ptrdiff_t>(r.frames * decoder.channels()));
        out.last = r.status;
        if (r.status == DecodeStatus::EndOfStream || r.status == DecodeStatus::Error) {
            break;
        }
        if (r.status == DecodeStatus::NeedMoreData) {
            if (source->snapshot().isComplete()) {
                break; // must not happen: a complete source never needs more data
            }
            grow();
        }
    }
    out.error = decoder.error();
    out.info = decoder.info();
    out.stats = decoder.stats();
    out.lengthFrames = decoder.lengthFrames();
    return out;
}

} // namespace selecta::testing
