// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/Decoder.h"

#include "audio/detail/Backend.h"
#include "audio/detail/ContainerProbe.h"
#include "audio/detail/FfmpegBackend.h"
#include "audio/detail/IndexedBackend.h"
#include "audio/detail/OutputStage.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

using namespace Qt::StringLiterals;

namespace selecta::audio {

using detail::Backend;

namespace {

constexpr qint64 kMaxGapFillSeconds = 10; // timeline gaps (lost frames) filled with silence up to this length
constexpr int kMinOutputRate = 8000;
constexpr int kMaxOutputRate = 384000;

const StreamInfo &emptyInfo()
{
    static const StreamInfo info;
    return info;
}

const Error &notOpenError()
{
    static const Error error{ErrorCode::InvalidArgument, QStringLiteral("the decoder is not open")};
    return error;
}

} // namespace

struct Decoder::Impl
{
    enum class State { Closed, Opening, Ready, Failed };

    std::shared_ptr<ByteSource> source;
    DecoderOptions options;
    bool headerOnly = false; // probeStreamInfo(): stream facts from the headers, no index, nothing decoded
    detail::ProbeResult probe;
    std::unique_ptr<Backend> backend;
    detail::OutputStage output;
    detail::DecodedChunk chunk;
    std::vector<float> silence;
    StreamInfo info;
    Error error;
    State state = State::Closed;
    std::vector<detail::DiscardWindow> windows;
    int sourceRate = 0;
    int sourceChannels = 0;
    int outRate = 0;
    int outChannels = 0;
    qint64 rawCursor = 0;
    bool cursorKnown = true;
    qint64 discardBefore = 0;
    qint64 position = 0;
    std::optional<qint64> pendingSeek;
    bool outputFinished = false;

    Decoder::OpenStatus fail(const Error &e)
    {
        error = e;
        state = State::Failed;
        backend.reset();
        return Decoder::OpenStatus::Failed;
    }

    Decoder::OpenStatus continueOpen();
    Decoder::OpenStatus finishOpen();
    void syncInfo();
    qint64 rawToOutput(qint64 raw) const;
    qint64 outputToRaw(qint64 out) const;
    qint64 toOutputRate(qint64 sourceFrames) const;
    qint64 toSourceRate(qint64 outFrames) const;
    Result<void> process(const detail::DecodedChunk &decoded);
    Result<void> pushKept(qint64 first, const float *data, qint64 count);
    DecodeStatus doSeek();
};

qint64 Decoder::Impl::rawToOutput(qint64 raw) const
{
    qint64 removed = 0;
    for (const detail::DiscardWindow &w : windows) {
        removed += std::clamp<qint64>(std::min(raw, w.end) - w.begin, 0, w.end - w.begin);
    }
    return std::max<qint64>(0, raw - removed);
}

qint64 Decoder::Impl::outputToRaw(qint64 out) const
{
    qint64 raw = out;
    for (const detail::DiscardWindow &w : windows) {
        if (raw >= w.begin) {
            raw += w.end - w.begin;
        } else {
            break;
        }
    }
    return raw;
}

qint64 Decoder::Impl::toOutputRate(qint64 sourceFrames) const
{
    if (outRate == sourceRate || sourceRate <= 0) {
        return sourceFrames;
    }
    return static_cast<qint64>(std::llround(static_cast<double>(sourceFrames) * outRate / sourceRate));
}

qint64 Decoder::Impl::toSourceRate(qint64 outFrames) const
{
    if (outRate == sourceRate || outRate <= 0) {
        return outFrames;
    }
    return static_cast<qint64>(std::floor(static_cast<double>(outFrames) * sourceRate / outRate));
}

void Decoder::Impl::syncInfo()
{
    if (!backend) {
        return;
    }
    info = backend->info();
    const qint64 rawLength = backend->rawLength();
    info.totalFrames = rawLength >= 0 ? rawToOutput(rawLength) : -1;
}

Decoder::OpenStatus Decoder::Impl::continueOpen()
{
    const SourceSnapshot snapshot = source->snapshot();
    detail::SourceReader reader(*source, snapshot);
    const detail::ParseResult probed = detail::probeContainer(reader, options.maxHeaderBytes, probe);
    if (probed.status == detail::ParseStatus::NeedMoreData) {
        if (snapshot.state == SourceState::Failed) {
            return fail(
                makeError(ErrorCode::SourceFailed, u"the download stopped before the file could be identified"_s));
        }
        return OpenStatus::NeedMoreData;
    }
    if (!probed.ok()) {
        return fail(makeError(detail::errorCodeFor(probed.status), probed.message));
    }

    const bool complete = snapshot.isComplete();
    bool own = false;
    switch (probe.container) {
    case Container::MpegAudio:
        backend = std::make_unique<detail::IndexedBackend>(detail::IndexedBackend::Kind::Mpeg, source, probe, options,
                                                           headerOnly);
        own = true;
        break;
    case Container::Flac:
        backend = std::make_unique<detail::IndexedBackend>(detail::IndexedBackend::Kind::Flac, source, probe, options,
                                                           headerOnly);
        own = true;
        break;
    case Container::Wav:
    case Container::Aiff:
        backend = std::make_unique<detail::IndexedBackend>(detail::IndexedBackend::Kind::Pcm, source, probe, options,
                                                           headerOnly);
        own = true;
        break;
    case Container::Mp4:
        if (!complete && probe.mp4MoovEnd < 0) {
            error = makeError(ErrorCode::NeedsCompleteFile, u"the MP4 index comes after the audio"_s);
            return OpenStatus::NeedsCompleteFile;
        }
        backend = std::make_unique<detail::FfmpegBackend>(source, probe, options, headerOnly);
        break;
    case Container::Ogg:
    case Container::Other:
    case Container::Unknown:
        if (!complete) {
            error = makeError(ErrorCode::NeedsCompleteFile, u"this format can only be decoded once complete"_s);
            return OpenStatus::NeedsCompleteFile;
        }
        backend = std::make_unique<detail::FfmpegBackend>(source, probe, options, headerOnly);
        break;
    }

    Error openError;
    Backend::Status status = backend->open(openError);
    if (status == Backend::Status::NeedMoreData) {
        backend.reset();
        return OpenStatus::NeedMoreData;
    }
    if (status == Backend::Status::Error) {
        backend.reset();
        if (openError.code == ErrorCode::NeedsCompleteFile) {
            error = std::move(openError);
            return OpenStatus::NeedsCompleteFile;
        }
        const bool fallback = own && (openError.code == ErrorCode::Unsupported || openError.code == ErrorCode::Corrupt);
        if (fallback && !complete) {
            // Selecta's parser doesn't handle this variant; FFmpeg may, once the file is complete.
            error = makeError(ErrorCode::NeedsCompleteFile, openError.message);
            return OpenStatus::NeedsCompleteFile;
        }
        if (!fallback) {
            return fail(openError);
        }
        qCDebug(lcAudio) << "falling back to FFmpeg's demuxer:" << openError.toString();
        backend = std::make_unique<detail::FfmpegBackend>(source, probe, options, headerOnly);
        Error ffmpegError;
        status = backend->open(ffmpegError);
        if (status != Backend::Status::Ok) {
            backend.reset();
            return fail(openError);
        }
    }
    return finishOpen();
}

Decoder::OpenStatus Decoder::Impl::finishOpen()
{
    const StreamInfo &backendInfo = backend->info();
    sourceRate = backendInfo.sampleRate;
    sourceChannels = backendInfo.channels;
    if (sourceRate <= 0 || sourceChannels <= 0) {
        return fail(makeError(ErrorCode::Corrupt, u"invalid sample rate or channel count"_s));
    }
    outRate = options.sampleRate > 0 ? options.sampleRate : sourceRate;
    outChannels = options.channels > 0 ? options.channels : sourceChannels;
    windows = backend->discardWindows();
    std::ranges::sort(windows, std::less<>{}, &detail::DiscardWindow::begin);
    syncInfo();
    if (info.totalFrames > options.maxDurationSeconds * sourceRate) {
        return fail(makeError(ErrorCode::TooLarge,
                              u"declared length of %1 hours exceeds the limit"_s.arg(
                                  static_cast<double>(info.totalFrames) / sourceRate / 3600.0, 0, 'f', 1)));
    }
    if (const Result<void> configured =
            output.configure(sourceRate, sourceChannels, outRate, outChannels, options.resampler);
        !configured) {
        return fail(configured.error());
    }
    rawCursor = 0;
    cursorKnown = true;
    discardBefore = 0;
    position = 0;
    pendingSeek.reset();
    outputFinished = false;
    error = {};
    state = State::Ready;
    return OpenStatus::Ready;
}

Result<void> Decoder::Impl::pushKept(qint64 first, const float *data, qint64 count)
{
    qint64 a = std::max(first, discardBefore);
    const qint64 end = first + count;
    for (const detail::DiscardWindow &w : windows) {
        if (a >= end) {
            break;
        }
        if (w.end <= a) {
            continue;
        }
        const qint64 keepEnd = std::min(end, w.begin);
        if (keepEnd > a) {
            if (Result<void> pushed = output.push(data + ((a - first) * sourceChannels), keepEnd - a); !pushed) {
                return pushed;
            }
        }
        a = std::max(a, w.end);
    }
    if (a < end) {
        return output.push(data + ((a - first) * sourceChannels), end - a);
    }
    return {};
}

Result<void> Decoder::Impl::process(const detail::DecodedChunk &decoded)
{
    qint64 first = decoded.firstSample;
    qint64 count = decoded.frames();
    const float *data = decoded.samples.data();
    if (count <= 0) {
        return {};
    }
    if (cursorKnown && first > rawCursor) {
        // Frames the codec didn't produce (corruption): silence keeps the timeline, within reason.
        const qint64 gap = first - rawCursor;
        if (gap <= kMaxGapFillSeconds * sourceRate) {
            silence.assign(static_cast<std::size_t>(gap * sourceChannels), 0.0F);
            if (Result<void> pushed = pushKept(rawCursor, silence.data(), gap); !pushed) {
                return pushed;
            }
        }
    } else if (cursorKnown && first < rawCursor) {
        const qint64 overlap = std::min(count, rawCursor - first);
        data += overlap * sourceChannels;
        first += overlap;
        count -= overlap;
        if (count == 0) {
            return {};
        }
    }
    rawCursor = first + count;
    cursorKnown = true;
    return pushKept(first, data, count);
}

DecodeStatus Decoder::Impl::doSeek()
{
    const qint64 target = pendingSeek.value_or(position);
    const qint64 raw = outputToRaw(toSourceRate(target));
    qint64 resumeAt = -1;
    Error seekError;
    switch (backend->seek(raw, resumeAt, seekError)) {
    case Backend::Status::Ok:
    case Backend::Status::EndOfStream:
        pendingSeek.reset();
        discardBefore = raw;
        rawCursor = resumeAt;
        cursorKnown = resumeAt >= 0;
        output.reset();
        outputFinished = false;
        return DecodeStatus::Ok;
    case Backend::Status::NeedMoreData:
        return DecodeStatus::NeedMoreData;
    case Backend::Status::Error:
        break;
    }
    fail(seekError);
    return DecodeStatus::Error;
}

// ---------------------------------------------------------------------------------------------------------------

Decoder::Decoder()
    : m_impl(std::make_unique<Impl>())
{}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder &&) noexcept = default;
Decoder &Decoder::operator=(Decoder &&) noexcept = default;

Decoder::OpenStatus Decoder::open(std::shared_ptr<ByteSource> source, const DecoderOptions &options)
{
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    close();
    Impl &d = *m_impl;
    if (!source) {
        return d.fail(makeError(ErrorCode::InvalidArgument, u"no source"_s));
    }
    if (options.channels < 0 || options.channels > 2 ||
        (options.sampleRate != 0 && (options.sampleRate < kMinOutputRate || options.sampleRate > kMaxOutputRate)) ||
        options.maxHeaderBytes <= 0 || options.maxDurationSeconds <= 0) {
        return d.fail(makeError(ErrorCode::InvalidArgument, u"invalid decoder options"_s));
    }
    detail::installFfmpegLogging();
    d.source = std::move(source);
    d.options = options;
    d.state = Impl::State::Opening;
    return d.continueOpen();
}

bool Decoder::isOpen() const
{
    return m_impl && m_impl->state == Impl::State::Ready;
}

void Decoder::close()
{
    if (!m_impl) {
        return;
    }
    Impl &d = *m_impl;
    d.backend.reset();
    d.source.reset();
    d.info = {};
    d.error = {};
    d.windows.clear();
    d.chunk.clear();
    d.output.reset();
    d.pendingSeek.reset();
    d.position = 0;
    d.state = Impl::State::Closed;
}

const StreamInfo &Decoder::info() const
{
    return isOpen() ? m_impl->info : emptyInfo();
}

int Decoder::sampleRate() const
{
    return isOpen() ? m_impl->outRate : 0;
}

int Decoder::channels() const
{
    return isOpen() ? m_impl->outChannels : 0;
}

qint64 Decoder::lengthFrames() const
{
    if (!isOpen()) {
        return -1;
    }
    m_impl->syncInfo();
    const qint64 total = m_impl->info.totalFrames;
    return total >= 0 ? m_impl->toOutputRate(total) : -1;
}

ReadResult Decoder::read(float *destination, qint64 maxFrames)
{
    if (!m_impl || m_impl->state != Impl::State::Ready) {
        if (m_impl && m_impl->state != Impl::State::Failed) {
            m_impl->error = notOpenError();
        }
        return {0, DecodeStatus::Error};
    }
    Impl &d = *m_impl;
    if (maxFrames <= 0) {
        return {0, DecodeStatus::Ok};
    }
    if (destination == nullptr) {
        d.error = makeError(ErrorCode::InvalidArgument, u"no destination buffer"_s);
        return {0, DecodeStatus::Error};
    }
    if (d.pendingSeek) {
        const DecodeStatus status = d.doSeek();
        if (status != DecodeStatus::Ok) {
            return {0, status};
        }
    }
    bool needMore = false;
    while (d.output.buffered() < maxFrames && !d.outputFinished && !needMore) {
        Error decodeError;
        switch (d.backend->decode(d.chunk, decodeError)) {
        case Backend::Status::Ok:
            if (const Result<void> processed = d.process(d.chunk); !processed) {
                d.fail(processed.error());
                return {0, DecodeStatus::Error};
            }
            break;
        case Backend::Status::NeedMoreData:
            needMore = true;
            break;
        case Backend::Status::EndOfStream:
            if (const Result<void> finished = d.output.finish(); !finished) {
                d.fail(finished.error());
                return {0, DecodeStatus::Error};
            }
            d.outputFinished = true;
            d.syncInfo();
            break;
        case Backend::Status::Error:
            d.fail(decodeError);
            return {0, DecodeStatus::Error};
        }
    }
    const qint64 frames = d.output.pull(destination, maxFrames);
    d.position += frames;
    DecodeStatus status = DecodeStatus::Ok;
    if (d.outputFinished && d.output.buffered() == 0) {
        status = DecodeStatus::EndOfStream;
    } else if (needMore && frames < maxFrames) {
        status = DecodeStatus::NeedMoreData;
    }
    return {frames, status};
}

qint64 Decoder::position() const
{
    if (!isOpen()) {
        return 0;
    }
    return m_impl->pendingSeek ? *m_impl->pendingSeek : m_impl->position;
}

DecodeStatus Decoder::seek(qint64 frame)
{
    if (!isOpen()) {
        if (m_impl && m_impl->state != Impl::State::Failed) {
            m_impl->error = notOpenError();
        }
        return DecodeStatus::Error;
    }
    Impl &d = *m_impl;
    frame = std::max<qint64>(0, frame);
    const qint64 length = lengthFrames();
    if (length >= 0 && d.info.exactLength) {
        frame = std::min(frame, length);
    }
    d.position = frame;
    d.output.reset();
    d.outputFinished = false;
    d.pendingSeek = frame;
    return d.doSeek();
}

qint64 Decoder::decodableFrames() const
{
    if (!isOpen()) {
        return 0;
    }
    Impl &d = *m_impl;
    qint64 raw = d.backend->decodableRawSamples();
    const qint64 rawLength = d.backend->rawLength();
    if (rawLength >= 0) {
        raw = std::min(raw, rawLength);
    }
    qint64 frames = d.toOutputRate(d.rawToOutput(raw));
    const qint64 length = lengthFrames();
    if (length >= 0) {
        frames = std::min(frames, length);
    }
    return frames;
}

DecoderStats Decoder::stats() const
{
    return isOpen() ? m_impl->backend->stats() : DecoderStats{};
}

const Error &Decoder::error() const
{
    return m_impl ? m_impl->error : notOpenError();
}

Result<std::unique_ptr<Decoder>> Decoder::clone(const DecoderOptions &options) const
{
    if (!isOpen()) {
        return makeError(ErrorCode::InvalidArgument, u"the decoder is not open"_s);
    }
    const Impl &d = *m_impl;
    if (!d.source->snapshot().isComplete()) {
        return makeError(ErrorCode::InvalidArgument, u"clone() needs a complete source"_s);
    }
    if (options.channels < 0 || options.channels > 2 ||
        (options.sampleRate != 0 && (options.sampleRate < kMinOutputRate || options.sampleRate > kMaxOutputRate))) {
        return makeError(ErrorCode::InvalidArgument, u"invalid decoder options"_s);
    }
    Result<std::unique_ptr<Backend>> backend = d.backend->clone();
    if (!backend) {
        return backend.error();
    }
    auto copy = std::make_unique<Decoder>();
    Impl &c = *copy->m_impl;
    c.source = d.source;
    c.options = options;
    c.probe = d.probe;
    c.backend = backend.takeValue();
    c.state = Impl::State::Opening;
    if (c.finishOpen() != OpenStatus::Ready) {
        return c.error;
    }
    return copy;
}

Result<StreamInfo> probeStreamInfo(std::shared_ptr<ByteSource> source, const DecoderOptions &options)
{
    if (!source) {
        return makeError(ErrorCode::InvalidArgument, u"no source"_s);
    }
    if (options.maxHeaderBytes <= 0 || options.maxDurationSeconds <= 0) {
        return makeError(ErrorCode::InvalidArgument, u"invalid probe options"_s);
    }
    detail::installFfmpegLogging();
    Decoder::Impl impl;
    impl.source = std::move(source);
    impl.options = options;
    impl.options.sampleRate = 0;
    impl.options.channels = 0;
    impl.headerOnly = true;
    impl.state = Decoder::Impl::State::Opening;
    switch (impl.continueOpen()) {
    case Decoder::OpenStatus::Ready:
        break;
    case Decoder::OpenStatus::Failed:
        return impl.error;
    case Decoder::OpenStatus::NeedMoreData:
        return makeError(ErrorCode::Corrupt, u"the file ends before its header does"_s);
    case Decoder::OpenStatus::NeedsCompleteFile:
        return impl.error.isError() ? impl.error
                                    : makeError(ErrorCode::NeedsCompleteFile, u"the file isn't complete"_s);
    }
    StreamInfo info = impl.info;
    info.mpeg.reset(); // nothing was indexed
    return info;
}

Result<StreamInfo> probeStreamInfo(const QString &path)
{
    Result<std::shared_ptr<FileSource>> source = FileSource::open(path);
    if (!source.ok()) {
        return source.error();
    }
    return probeStreamInfo(source.takeValue());
}

} // namespace selecta::audio
