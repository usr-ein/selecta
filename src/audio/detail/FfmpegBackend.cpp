// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/FfmpegBackend.h"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <climits>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {
namespace {

constexpr int kProbeBytes = 1024 * 1024; // probesize: 1 MB (audio §11.2)
constexpr int kMaxPacketsPerDecode = 256;
constexpr qint64 kSeekPrerollSamples = 8192;      // AAC needs 2 112 (audio §5.2); generous for other codecs
constexpr qint64 kOpenMargin = qint64{64} * 1024; // bytes past `moov` a growing MP4 needs before opening
constexpr int kMaxStreams = 32;

Container containerFromFormat(const AVInputFormat *format, Container probed)
{
    if (probed != Container::Unknown) {
        return probed;
    }
    if (format == nullptr || format->name == nullptr) {
        return Container::Other;
    }
    const QByteArray name(format->name);
    if (name.contains("mov") || name.contains("mp4")) {
        return Container::Mp4;
    }
    if (name == "ogg") {
        return Container::Ogg;
    }
    if (name == "mp3") {
        return Container::MpegAudio;
    }
    if (name == "flac") {
        return Container::Flac;
    }
    if (name == "wav") {
        return Container::Wav;
    }
    if (name == "aiff") {
        return Container::Aiff;
    }
    return Container::Other;
}

Codec codecFromId(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AAC:
        return Codec::Aac;
    case AV_CODEC_ID_ALAC:
        return Codec::Alac;
    case AV_CODEC_ID_VORBIS:
        return Codec::Vorbis;
    case AV_CODEC_ID_OPUS:
        return Codec::Opus;
    case AV_CODEC_ID_MP3:
        return Codec::Mp3;
    case AV_CODEC_ID_MP2:
        return Codec::Mp2;
    case AV_CODEC_ID_MP1:
        return Codec::Mp1;
    case AV_CODEC_ID_FLAC:
        return Codec::Flac;
    default:
        break;
    }
    const AVCodecDescriptor *descriptor = avcodec_descriptor_get(id);
    if (descriptor != nullptr && descriptor->name != nullptr && QByteArray(descriptor->name).startsWith("pcm_")) {
        return Codec::Pcm;
    }
    return Codec::Other;
}

} // namespace

FfmpegBackend::FfmpegBackend(std::shared_ptr<ByteSource> source, const ProbeResult &probe,
                             const DecoderOptions &options, bool headerOnly)
    : m_source(std::move(source))
    , m_probe(probe)
    , m_options(options)
    , m_headerOnly(headerOnly)
{}

FfmpegBackend::~FfmpegBackend()
{
    m_format.reset(); // before the AVIOContext it reads from
    m_avio.reset();
}

Backend::Status FfmpegBackend::outOfData(Error &error) const
{
    if (m_source->state() == SourceState::Failed) {
        error = makeError(ErrorCode::SourceFailed, u"the download stopped before the audio needed"_s);
        return Status::Error;
    }
    return Status::NeedMoreData;
}

Backend::Status FfmpegBackend::open(Error &error)
{
    installFfmpegLogging();
    m_format.reset();
    m_avio.reset();
    m_hasPrimed = false;
    m_originPts = AV_NOPTS_VALUE;
    m_nextRaw = 0;
    m_nextEntry = 0;
    m_drained = false;

    const SourceSnapshot snapshot = m_source->snapshot();
    m_growing = !snapshot.isComplete();
    if (m_growing) {
        if (m_probe.container != Container::Mp4 || m_probe.mp4MoovEnd < 0) {
            error = makeError(ErrorCode::NeedsCompleteFile, u"this file can only be decoded once complete"_s);
            return Status::Error;
        }
        const qint64 needed = std::min(m_source->totalSize(), m_probe.mp4MoovEnd + kOpenMargin);
        if (snapshot.available < needed) {
            return outOfData(error);
        }
    }

    m_avio = std::make_unique<AvioAdapter>(m_source);
    if (const Result<void> created = m_avio->create(); !created) {
        error = created.error();
        return Status::Error;
    }
    AVFormatContext *context = avformat_alloc_context();
    if (context == nullptr) {
        error = makeError(ErrorCode::Internal, u"out of memory allocating a demuxer"_s);
        return Status::Error;
    }
    context->pb = m_avio->context();
    context->flags = withFlag(context->flags, AVFMT_FLAG_CUSTOM_IO);
    context->probesize = kProbeBytes;
    context->format_probesize = kProbeBytes;
    context->max_analyze_duration = AV_TIME_BASE;
    context->max_streams = kMaxStreams;
    const AVInputFormat *input = nullptr;
    if (m_probe.container == Container::Mp4) {
        input = av_find_input_format("mov");
    } else if (m_probe.container == Container::Ogg) {
        input = av_find_input_format("ogg");
    }
    int ret = avformat_open_input(&context, "", input, nullptr);
    if (ret < 0) {
        // avformat_open_input freed the context, not our AVIOContext.
        if (m_avio->takeStarved()) {
            return outOfData(error);
        }
        if (m_avio->readFailed()) {
            error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
        } else {
            error = makeError(ErrorCode::Unsupported,
                              u"not an audio file FFmpeg can read (%1)"_s.arg(ffmpegErrorString(ret)));
        }
        return Status::Error;
    }
    m_format.reset(context);
    // Without decoding a first frame nothing would catch a weak guess on data that isn't audio: a header-only probe
    // takes only formats FFmpeg recognised with confidence (MP4 and Ogg are named, not guessed).
    if (m_headerOnly && input == nullptr && context->probe_score <= AVPROBE_SCORE_RETRY) {
        error = makeError(ErrorCode::Unsupported, u"not an audio file FFmpeg recognises with confidence"_s);
        return Status::Error;
    }
    // A header-only probe skips FFmpeg's stream analysis (which decodes) when the header already gave the audio
    // parameters, as it does for MP4, Ogg, ASF, APE and WavPack.
    const bool parametersKnown = [context] {
        for (unsigned i = 0; i < context->nb_streams; ++i) {
            const AVCodecParameters *p = context->streams[i]->codecpar;
            if (p->codec_type == AVMEDIA_TYPE_AUDIO && p->sample_rate > 0 && p->ch_layout.nb_channels > 0) {
                return true;
            }
        }
        return false;
    }();
    if (!m_growing && !(m_headerOnly && parametersKnown)) {
        ret = avformat_find_stream_info(context, nullptr);
        if (ret < 0) {
            error =
                makeError(ErrorCode::Corrupt, u"can't read the stream parameters (%1)"_s.arg(ffmpegErrorString(ret)));
            return Status::Error;
        }
    }
    ret = av_find_best_stream(context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ret < 0) {
        error = makeError(ErrorCode::Unsupported, u"no audio stream"_s);
        return Status::Error;
    }
    m_stream = ret;
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        context->streams[i]->discard = std::cmp_equal(i, m_stream) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    const AVStream *stream = context->streams[m_stream];
    m_timeBase = stream->time_base;
    const AVCodecParameters *parameters = stream->codecpar;
    if (parameters->sample_rate <= 0 || parameters->sample_rate > 768000 || parameters->ch_layout.nb_channels <= 0 ||
        parameters->ch_layout.nb_channels > 32 || m_timeBase.num <= 0 || m_timeBase.den <= 0) {
        error = makeError(ErrorCode::Corrupt, u"invalid stream parameters"_s);
        return Status::Error;
    }
    if (const Result<void> opened = m_codec.open(parameters, m_timeBase, false); !opened) {
        error = opened.error();
        return Status::Error;
    }
    m_packet.reset(av_packet_alloc());
    if (!m_packet) {
        error = makeError(ErrorCode::Internal, u"out of memory allocating a packet"_s);
        return Status::Error;
    }
    fillInfo();
    if (m_headerOnly) {
        return Status::Ok; // stream facts only: nothing is decoded
    }

    // Decode the first frame: its timestamp is the origin of the raw timeline.
    m_findingOrigin = true;
    const Status primed = readAndDecode(m_primed, error);
    m_findingOrigin = false;
    if (primed == Status::NeedMoreData || primed == Status::Error) {
        return primed;
    }
    m_hasPrimed = primed == Status::Ok && m_primed.frames() > 0;
    m_info.exactSeek = m_originPts != AV_NOPTS_VALUE;
    return Status::Ok;
}

void FfmpegBackend::fillInfo()
{
    const AVStream *stream = m_format->streams[m_stream];
    const AVCodecParameters *parameters = stream->codecpar;
    m_info.container = containerFromFormat(m_format->iformat, m_probe.container);
    m_info.codec = codecFromId(parameters->codec_id);
    m_info.codecName = m_codec.codecName();
    m_info.sampleRate = parameters->sample_rate;
    m_info.channels = parameters->ch_layout.nb_channels;
    const AVCodecDescriptor *descriptor = avcodec_descriptor_get(parameters->codec_id);
    m_info.lossless = descriptor != nullptr && hasFlag(descriptor->props, AV_CODEC_PROP_LOSSLESS);
    if (m_info.lossless) {
        // The container's figure, else the opened decoder's (ALAC's depth is in its magic cookie, which only the
        // decoder reads), else the PCM codec's own width.
        const AVCodecContext *codec = m_codec.context();
        if (parameters->bits_per_raw_sample > 0) {
            m_info.bitsPerSample = parameters->bits_per_raw_sample;
        } else if (codec != nullptr && codec->bits_per_raw_sample > 0) {
            m_info.bitsPerSample = codec->bits_per_raw_sample;
        } else {
            m_info.bitsPerSample = av_get_bits_per_sample(parameters->codec_id);
        }
    }
    m_info.floatingPoint =
        parameters->codec_id == AV_CODEC_ID_PCM_F32LE || parameters->codec_id == AV_CODEC_ID_PCM_F32BE ||
        parameters->codec_id == AV_CODEC_ID_PCM_F64LE || parameters->codec_id == AV_CODEC_ID_PCM_F64BE;
    const qint64 bitRate = parameters->bit_rate > 0 ? parameters->bit_rate : m_format->bit_rate;
    m_info.bitrateKbps = bitRate > 0 ? static_cast<int>(bitRate / 1000) : 0;
    m_info.exactLength = m_info.container == Container::Mp4 && stream->duration != AV_NOPTS_VALUE;
    m_info.progressive = m_info.container == Container::Mp4 && m_probe.mp4MoovEnd >= 0;
    m_info.audioOffset = 0;
    m_info.audioBytes = -1;
}

qint64 FfmpegBackend::rawFromPts(qint64 pts) const
{
    return av_rescale_q(pts - m_originPts, m_timeBase, AVRational{1, m_info.sampleRate});
}

qint64 FfmpegBackend::ptsFromRaw(qint64 raw) const
{
    const qint64 origin = m_originPts != AV_NOPTS_VALUE ? m_originPts : 0;
    return av_rescale_q(raw, AVRational{1, m_info.sampleRate}, m_timeBase) + origin;
}

bool FfmpegBackend::gateNextPacket()
{
    const SourceSnapshot snapshot = m_source->snapshot();
    if (snapshot.isComplete()) {
        m_growing = false;
        m_avio->refreshSeekable();
        return true;
    }
    AVStream *stream = m_format->streams[m_stream];
    const int count = avformat_index_get_entries_count(stream);
    if (m_nextEntry >= count) {
        return true; // no further sample: the demuxer ends without reading
    }
    const AVIndexEntry *entry = avformat_index_get_entry(stream, m_nextEntry);
    return entry != nullptr && entry->pos + entry->size <= snapshot.available;
}

Backend::Status FfmpegBackend::endOfPackets(int ret, DecodedChunk &chunk, Error &error)
{
    if (m_avio->readFailed()) {
        error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
        return Status::Error;
    }
    if (ret != ffErrorEof()) {
        qCDebug(lcAudio) << "demuxing stopped early:" << ffmpegErrorString(ret);
        m_truncated = true;
    }
    if (m_drained) {
        return Status::EndOfStream;
    }
    m_drained = true;
    qint64 pts = AV_NOPTS_VALUE;
    const Result<qint64> drained = m_codec.drain(chunk.samples, &pts);
    if (!drained || drained.value() == 0) {
        chunk.samples.clear();
        return Status::EndOfStream;
    }
    chunk.firstSample = pts != AV_NOPTS_VALUE && m_originPts != AV_NOPTS_VALUE ? rawFromPts(pts) : m_nextRaw;
    m_nextRaw = chunk.firstSample + drained.value();
    return Status::Ok;
}

void FfmpegBackend::trackPacketEntry()
{
    // Find the index entry of the packet just read, to gate the next one.
    AVStream *stream = m_format->streams[m_stream];
    const int count = avformat_index_get_entries_count(stream);
    int found = -1;
    for (int i = std::max(0, m_nextEntry - 1); i < count && i < m_nextEntry + 64; ++i) {
        const AVIndexEntry *entry = avformat_index_get_entry(stream, i);
        if (entry != nullptr && entry->pos == m_packet->pos) {
            found = i;
            break;
        }
    }
    if (found < 0 && m_packet->dts != AV_NOPTS_VALUE) {
        found = av_index_search_timestamp(stream, m_packet->dts, withFlag(AVSEEK_FLAG_BACKWARD, AVSEEK_FLAG_ANY));
    }
    m_nextEntry = found >= 0 ? found + 1 : m_nextEntry + 1;
}

bool FfmpegBackend::dropBeforeTarget(DecodedChunk &chunk, qint64 frames)
{
    if (m_dropBefore < 0) {
        return true;
    }
    const qint64 raw = chunk.firstSample;
    const qint64 drop = std::clamp<qint64>(m_dropBefore - raw, 0, frames);
    if (drop > 0) {
        chunk.samples.erase(chunk.samples.begin(),
                            chunk.samples.begin() + static_cast<std::ptrdiff_t>(drop * chunk.channels));
        chunk.firstSample += drop;
    }
    if (raw + frames >= m_dropBefore) {
        m_dropBefore = -1;
    }
    return chunk.frames() > 0;
}

Backend::Status FfmpegBackend::resumeAfterStarvation(Error &error)
{
    const SourceSnapshot snapshot = m_source->snapshot();
    if (!snapshot.isComplete() && snapshot.available <= m_starvedAt) {
        return outOfData(error);
    }
    const qint64 resume = m_repositionRaw;
    const Status status = reposition(resume, error);
    if (status == Status::Ok) {
        m_dropBefore = resume;
    }
    return status;
}

Backend::Status FfmpegBackend::readAndDecode(DecodedChunk &chunk, Error &error)
{
    chunk.clear();
    chunk.channels = m_info.channels;
    if (m_needReposition) {
        if (const Status status = resumeAfterStarvation(error); status != Status::Ok) {
            return status;
        }
    }
    for (int packets = 0; packets < kMaxPacketsPerDecode; ++packets) {
        if (m_growing && !gateNextPacket()) {
            return outOfData(error);
        }
        av_packet_unref(m_packet.get());
        const int ret = av_read_frame(m_format.get(), m_packet.get());
        if (m_avio->takeStarved()) {
            av_packet_unref(m_packet.get());
            m_needReposition = true;
            m_repositionRaw = m_nextRaw;
            m_starvedAt = m_avio->starvedAt();
            return outOfData(error);
        }
        if (ret < 0) {
            return endOfPackets(ret, chunk, error);
        }
        if (m_packet->stream_index != m_stream) {
            continue;
        }
        if (m_growing) {
            trackPacketEntry();
        }
        qint64 pts = AV_NOPTS_VALUE;
        const std::size_t before = chunk.samples.size();
        const Result<qint64> decoded = m_codec.decode(m_packet.get(), chunk.samples, &pts);
        av_packet_unref(m_packet.get());
        if (!decoded) {
            ++m_decodeErrors;
            chunk.samples.resize(before);
            continue;
        }
        const qint64 frames = decoded.value();
        if (frames == 0) {
            continue;
        }
        if (m_findingOrigin) {
            m_findingOrigin = false;
            m_originPts = pts; // AV_NOPTS_VALUE: positions are counted instead
        }
        chunk.firstSample = pts != AV_NOPTS_VALUE && m_originPts != AV_NOPTS_VALUE ? rawFromPts(pts) : m_nextRaw;
        m_nextRaw = chunk.firstSample + frames;
        if (dropBeforeTarget(chunk, frames)) {
            return Status::Ok;
        }
    }
    return Status::Ok; // many packets without output (priming): an empty chunk, the caller asks again
}

Backend::Status FfmpegBackend::decode(DecodedChunk &chunk, Error &error)
{
    if (m_hasPrimed) {
        m_hasPrimed = false;
        chunk = std::move(m_primed);
        m_primed = {};
        return Status::Ok;
    }
    return readAndDecode(chunk, error);
}

Backend::Status FfmpegBackend::reposition(qint64 rawSample, Error &error)
{
    const qint64 target = std::max<qint64>(0, rawSample - kSeekPrerollSamples);
    const qint64 timestamp = ptsFromRaw(target);
    AVStream *stream = m_format->streams[m_stream];
    if (m_growing && m_source->snapshot().isComplete()) {
        m_growing = false;
        m_avio->refreshSeekable();
    }
    int entry = 0;
    if (m_growing) {
        entry = std::max(0, av_index_search_timestamp(stream, timestamp, AVSEEK_FLAG_BACKWARD));
        const AVIndexEntry *e = avformat_index_get_entry(stream, entry);
        if (e == nullptr || e->pos + e->size > m_source->snapshot().available) {
            return outOfData(error);
        }
    }
    int ret = av_seek_frame(m_format.get(), m_stream, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ret = avformat_seek_file(m_format.get(), m_stream, INT64_MIN, timestamp, timestamp, 0);
    }
    if (m_avio->takeStarved()) {
        m_needReposition = true;
        m_repositionRaw = rawSample;
        m_starvedAt = m_avio->starvedAt();
        return outOfData(error);
    }
    if (ret < 0) {
        error = makeError(ErrorCode::DecoderError, u"can't seek (%1)"_s.arg(ffmpegErrorString(ret)));
        return Status::Error;
    }
    m_codec.flush();
    m_drained = false;
    m_hasPrimed = false;
    m_needReposition = false;
    m_nextRaw = target;
    m_nextEntry = entry;
    return Status::Ok;
}

Backend::Status FfmpegBackend::seek(qint64 rawSample, qint64 &resumeAt, Error &error)
{
    resumeAt = -1;
    m_dropBefore = -1;
    return reposition(std::max<qint64>(0, rawSample), error);
}

qint64 FfmpegBackend::rawLength() const
{
    if (!m_format || m_stream < 0) {
        return -1;
    }
    const AVStream *stream = m_format->streams[m_stream];
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const qint64 start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
        const qint64 origin = m_originPts != AV_NOPTS_VALUE ? m_originPts : start;
        const qint64 endPts = start + stream->duration;
        return std::max<qint64>(0, av_rescale_q(endPts - origin, m_timeBase, AVRational{1, m_info.sampleRate}));
    }
    if (m_format->duration > 0) {
        return av_rescale(m_format->duration, m_info.sampleRate, AV_TIME_BASE);
    }
    return -1;
}

qint64 FfmpegBackend::decodableRawSamples()
{
    const SourceSnapshot snapshot = m_source->snapshot();
    if (!m_growing || snapshot.isComplete()) {
        const qint64 length = rawLength();
        return length >= 0 ? length : m_nextRaw;
    }
    AVStream *stream = m_format->streams[m_stream];
    const int count = avformat_index_get_entries_count(stream);
    // Entries of a single audio track are in file order: binary search the last one fully downloaded.
    int low = 0;
    int high = count;
    while (low < high) {
        const int mid = low + ((high - low) / 2);
        const AVIndexEntry *entry = avformat_index_get_entry(stream, mid);
        if (entry != nullptr && entry->pos + entry->size <= snapshot.available) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low <= 0) {
        return 0;
    }
    if (low >= count) {
        const qint64 length = rawLength();
        return length >= 0 ? length : m_nextRaw;
    }
    const AVIndexEntry *next = avformat_index_get_entry(stream, low);
    return next != nullptr ? std::max<qint64>(0, rawFromPts(next->timestamp)) : 0;
}

DecoderStats FfmpegBackend::stats() const
{
    DecoderStats stats;
    stats.decodeErrors = m_decodeErrors;
    stats.truncated = m_truncated;
    return stats;
}

Result<std::unique_ptr<Backend>> FfmpegBackend::clone() const
{
    auto copy = std::make_unique<FfmpegBackend>(m_source, m_probe, m_options);
    Error error;
    if (copy->open(error) != Status::Ok) {
        if (!error.isError()) {
            error = makeError(ErrorCode::InvalidArgument, u"the source is not complete"_s);
        }
        return error;
    }
    return std::unique_ptr<Backend>(std::move(copy));
}

} // namespace selecta::audio::detail
