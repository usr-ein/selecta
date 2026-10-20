// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/Transcoder.h"

#include "audio/ByteSource.h"
#include "audio/Decoder.h"
#include "audio/detail/ThreadHandOver.h"
#include "convert/detail/Encoder.h"
#include "convert/detail/OutputFile.h"
#include "convert/detail/PcmHash.h"
#include "convert/detail/TranscodeSeam.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

constexpr qint64 kChunkFrames = 4096;
constexpr qint64 kCopyChunkBytes = qint64{1024} * 1024;
constexpr qint64 kMp3FrameSamples = 1152; // the verifier's tolerance for LAME outputs (conversion §2.5)
// Shares of the progress bar: encoding (or copying and decoding the source) first, then tags and verification.
constexpr double kEncodeShare = 0.9;
constexpr double kCopyShare = 0.3;
constexpr double kSourceCheckShare = 0.6;
constexpr double kProgressStep = 0.005;
constexpr auto kJobProgressInterval = std::chrono::milliseconds(100);

bool isCancelled(const audio::CancellationFlag *cancel)
{
    return cancel != nullptr && cancel->isCancelled();
}

Error cancelledError()
{
    return makeError(ErrorCode::Cancelled, u"the conversion was cancelled"_s);
}

QSize imageSize(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    return QImageReader(&buffer).size(); // the header only
}

// Calls the caller's callback when the fraction moved by half a percent or more, and once at 1.
class ProgressReporter
{
public:
    explicit ProgressReporter(const TranscodeProgress &callback)
        : m_callback(callback)
    {}

    void report(double fraction)
    {
        if (!m_callback) {
            return;
        }
        fraction = std::clamp(fraction, 0.0, 1.0);
        if (fraction - m_last >= kProgressStep || (fraction >= 1.0 && m_last < 1.0)) {
            m_last = fraction;
            m_callback(fraction);
        }
    }

private:
    const TranscodeProgress &m_callback;
    double m_last = -1.0;
};

audio::Container outputContainer(OutputFormat format, audio::Container source)
{
    switch (format) {
    case OutputFormat::Mp3:
        return audio::Container::MpegAudio;
    case OutputFormat::Aiff:
        return audio::Container::Aiff;
    case OutputFormat::Flac:
        return audio::Container::Flac;
    case OutputFormat::Wav:
        return audio::Container::Wav;
    case OutputFormat::Alac:
        return audio::Container::Mp4;
    case OutputFormat::KeepAudio:
        break;
    }
    return source;
}

// What every step of one conversion shares.
struct Job
{
    Job(const TranscodeRequest &request, const audio::CancellationFlag *cancel, ProgressReporter &progress,
        const detail::DeviceWrapper &wrapDevice)
        : request(request)
        , cancel(cancel)
        , progress(progress)
        , wrapDevice(wrapDevice)
    {}

    const TranscodeRequest &request;
    const audio::CancellationFlag *cancel = nullptr;
    ProgressReporter &progress;
    const detail::DeviceWrapper &wrapDevice;
    audio::StreamInfo source;
    ResolvedOutput resolved;
    MetadataModel tags; // after the tag policy
};

Result<void> validate(const TranscodeRequest &request)
{
    if (const Result<void> settings = checkSettings(request.settings); !settings.ok()) {
        return settings;
    }
    if (!QFileInfo(request.sourcePath).isFile()) {
        return makeError(ErrorCode::OpenFailed, u"%1 is not a readable file"_s.arg(request.sourcePath));
    }
    if (const std::optional<Picture> cover = request.tags.frontCover(); cover && !imageSize(cover->data).isValid()) {
        return makeError(ErrorCode::InvalidArgument, u"the front cover isn't an image Selecta can read"_s);
    }
    return {};
}

Result<std::unique_ptr<audio::Decoder>> openSource(const QString &path)
{
    Result<std::shared_ptr<audio::FileSource>> source = audio::FileSource::open(path);
    if (!source.ok()) {
        return source.error();
    }
    audio::DecoderOptions options;
    options.channels = 0;
    auto decoder = std::make_unique<audio::Decoder>();
    switch (decoder->open(source.takeValue(), options)) {
    case audio::Decoder::OpenStatus::Ready:
        return decoder;
    case audio::Decoder::OpenStatus::Failed:
        return decoder->error();
    case audio::Decoder::OpenStatus::NeedMoreData:
    case audio::Decoder::OpenStatus::NeedsCompleteFile:
        break;
    }
    return makeError(ErrorCode::Internal, u"the decoder wants more of %1, which is complete"_s.arg(path));
}

// The tags written: the request's model through the policy, with Selecta's encoder settings for re-encoded audio.
MetadataModel policyTags(const TranscodeRequest &request)
{
    TagPolicyOptions policy = request.policy;
    policy.transcoding = request.settings.format != OutputFormat::KeepAudio;
    if (policy.transcoding && policy.encoderSettings.isEmpty() && request.settings.format == OutputFormat::Mp3) {
        policy.encoderSettings = lameSettingsString(request.settings.mp3);
    }
    return applyTagPolicy(request.tags, policy);
}

VerifyExpectations tagExpectations(const Job &job, audio::Container container)
{
    VerifyExpectations expectations;
    expectations.container = container;
    expectations.expectedTags = tagsAsWritten(job.tags, container, job.request.tagOptions);
    if (const std::optional<Picture> cover = job.tags.frontCover()) {
        expectations.expectFrontCover = true;
        expectations.expectedCoverSize = imageSize(cover->data);
    }
    return expectations;
}

// Tags, verification, sync and publication: the common end of both paths.
Result<TranscodeReport> finishOutput(detail::TempOutput &temp, const Job &job, const VerifyExpectations &expectations,
                                     TranscodeReport report)
{
    if (const Result<void> tagged = writeTags(temp.path(), job.tags, job.request.tagOptions); !tagged.ok()) {
        return tagged.error();
    }
    if (isCancelled(job.cancel)) {
        return cancelledError();
    }
    Result<VerifyReport> verified = verifyOutput(temp.path(), expectations, job.cancel);
    if (!verified.ok()) {
        if (verified.error().code == ErrorCode::Cancelled) {
            return verified.error();
        }
        return makeError(ErrorCode::VerificationFailed,
                         u"the output can't be examined: %1"_s.arg(verified.error().message));
    }
    report.verification = verified.takeValue();
    if (!report.verification.passed()) {
        return makeError(ErrorCode::VerificationFailed,
                         u"the output of %1 failed verification: %2"_s.arg(job.request.sourcePath,
                                                                           report.verification.problems.join(u"; "_s)));
    }
    if (isCancelled(job.cancel)) {
        return cancelledError();
    }
    if (const Result<void> synced = temp.sync(); !synced.ok()) {
        return synced.error();
    }
    if (const Result<void> published = temp.publish(); !published.ok()) {
        return published.error();
    }
    report.outputPath = temp.finalPath();
    report.outputBytes = QFileInfo(temp.finalPath()).size();
    job.progress.report(1.0);
    return report;
}

Result<void> checkSourceHealth(const audio::DecoderStats &stats, const QString &path)
{
    if (stats.decodeErrors == 0 && !stats.truncated) {
        return {};
    }
    const QString damage = stats.truncated ? u"it ends before its header says it should"_s
                                           : u"%1 packets don't decode"_s.arg(stats.decodeErrors);
    return makeError(ErrorCode::Corrupt,
                     u"%1 is damaged (%2); it can still be filed as it is, or downloaded again"_s.arg(path, damage));
}

Result<TranscodeReport> encode(Job &job, std::unique_ptr<audio::Decoder> decoder)
{
    const audio::StreamInfo &info = job.source;
    const ResolvedOutput &out = job.resolved;
    audio::DecoderOptions options;
    options.sampleRate = out.resample ? out.sampleRate : 0;
    options.channels = info.channels > 2 ? 2 : 0; // a DJ library is stereo: surround is downmixed
    options.resampler = audio::ResamplerQuality::VeryHigh;
    if (options.sampleRate != 0 || options.channels != 0) {
        Result<std::unique_ptr<audio::Decoder>> cloned = decoder->clone(options);
        if (!cloned.ok()) {
            return cloned.error();
        }
        decoder = cloned.takeValue();
    }
    const detail::EncoderFormat format{.sampleRate = decoder->sampleRate(),
                                       .channels = decoder->channels(),
                                       .bitDepth = out.bitDepth,
                                       .dither = out.dither};
    // Lossless at the source's own rate, channels and (at least) word length: the output must decode to exactly the
    // samples that went in.
    const bool bitExact = format.bitDepth > 0 && info.lossless && !info.floatingPoint && info.bitsPerSample > 0 &&
                          info.bitsPerSample <= format.bitDepth && !out.resample && !out.dither &&
                          options.channels == 0;

    Result<std::unique_ptr<detail::TempOutput>> created = detail::TempOutput::create(job.request.outputPath);
    if (!created.ok()) {
        return created.error();
    }
    const std::unique_ptr<detail::TempOutput> temp = created.takeValue();
    std::unique_ptr<QIODevice> wrapped = job.wrapDevice ? job.wrapDevice(temp->file()) : nullptr;
    detail::Sink sink(wrapped ? wrapped.get() : &temp->file(), temp->path());
    Result<std::unique_ptr<detail::Encoder>> made = detail::createEncoder(job.request.settings, format, sink);
    if (!made.ok()) {
        return made.error();
    }
    std::unique_ptr<detail::Encoder> encoder = made.takeValue();

    QCryptographicHash digest(QCryptographicHash::Md5);
    std::vector<float> buffer(static_cast<std::size_t>(kChunkFrames * format.channels));
    const qint64 length = decoder->lengthFrames();
    qint64 frames = 0;
    while (true) {
        if (isCancelled(job.cancel)) {
            return cancelledError();
        }
        const audio::ReadResult read = decoder->read(buffer.data(), kChunkFrames);
        if (read.frames > 0) {
            if (bitExact) {
                detail::addFloatSamples(digest, buffer.data(), read.frames * format.channels);
            }
            if (const Result<void> written = encoder->write(buffer.data(), read.frames); !written.ok()) {
                return written.error();
            }
            frames += read.frames;
            if (length > 0) {
                job.progress.report(kEncodeShare * static_cast<double>(frames) / static_cast<double>(length));
            }
        }
        if (read.status == audio::DecodeStatus::EndOfStream) {
            break;
        }
        if (read.status == audio::DecodeStatus::Error) {
            return decoder->error();
        }
        if (read.status == audio::DecodeStatus::NeedMoreData) {
            return makeError(ErrorCode::Internal, u"the decoder wants more of a complete file"_s);
        }
    }
    if (const Result<void> healthy = checkSourceHealth(decoder->stats(), job.request.sourcePath); !healthy.ok()) {
        return healthy.error();
    }
    if (const Result<void> finished = encoder->finish(); !finished.ok()) {
        return finished.error();
    }
    if (encoder->clippedSamples() > 0) {
        qCDebug(lcConvert) << encoder->clippedSamples() << "samples clipped while converting" << job.request.sourcePath;
    }
    encoder.reset();
    wrapped.reset();
    if (const Result<void> closed = temp->closeFile(); !closed.ok()) {
        return closed.error();
    }

    const bool mp3 = job.request.settings.format == OutputFormat::Mp3;
    VerifyExpectations expectations =
        tagExpectations(job, outputContainer(job.request.settings.format, info.container));
    expectations.sampleRate = format.sampleRate;
    expectations.channels = format.channels;
    expectations.bitsPerSample = format.bitDepth;
    expectations.expectedFrames = frames;
    expectations.frameTolerance = mp3 ? kMp3FrameSamples : 0;
    if (bitExact) {
        expectations.expectedPcmDigest = digest.result();
    }
    if (mp3) {
        expectations.mp3CbrKbps =
            job.request.settings.mp3.mode == Mp3Mode::Cbr ? job.request.settings.mp3.bitrateKbps : 0;
        expectations.requireLameTag = true;
    }

    TranscodeReport report;
    report.source = info;
    report.outputSampleRate = format.sampleRate;
    report.outputBitDepth = format.bitDepth;
    report.resampled = out.resample;
    report.dithered = out.dither;
    return finishOutput(*temp, job, expectations, std::move(report));
}

Result<void> copyFile(const Job &job, detail::Sink &sink)
{
    QFile in(job.request.sourcePath);
    if (!in.open(QIODevice::ReadOnly)) {
        return makeError(ErrorCode::OpenFailed, u"can't read %1: %2"_s.arg(job.request.sourcePath, in.errorString()));
    }
    const qint64 total = in.size();
    qint64 copied = 0;
    QByteArray chunk(static_cast<qsizetype>(kCopyChunkBytes), Qt::Uninitialized);
    while (true) {
        if (isCancelled(job.cancel)) {
            return cancelledError();
        }
        const qint64 read = in.read(chunk.data(), chunk.size());
        if (read < 0) {
            return makeError(ErrorCode::ReadFailed,
                             u"can't read %1: %2"_s.arg(job.request.sourcePath, in.errorString()));
        }
        if (read == 0) {
            break;
        }
        if (const Result<void> written = sink.write(chunk.constData(), read); !written.ok()) {
            return written;
        }
        copied += read;
        if (total > 0) {
            job.progress.report(kCopyShare * static_cast<double>(copied) / static_cast<double>(total));
        }
    }
    if (copied != total) {
        return makeError(ErrorCode::ReadFailed, u"%1 changed while it was copied"_s.arg(job.request.sourcePath));
    }
    return {};
}

// CONV-2 "file as-is": the audio bytes copied untouched, the tags rewritten, the decoded audio proven identical.
Result<TranscodeReport> keepAudio(Job &job)
{
    Result<std::unique_ptr<detail::TempOutput>> created = detail::TempOutput::create(job.request.outputPath);
    if (!created.ok()) {
        return created.error();
    }
    const std::unique_ptr<detail::TempOutput> temp = created.takeValue();
    std::unique_ptr<QIODevice> wrapped = job.wrapDevice ? job.wrapDevice(temp->file()) : nullptr;
    detail::Sink sink(wrapped ? wrapped.get() : &temp->file(), temp->path());
    if (const Result<void> copied = copyFile(job, sink); !copied.ok()) {
        return copied.error();
    }
    wrapped.reset();
    if (const Result<void> closed = temp->closeFile(); !closed.ok()) {
        return closed.error();
    }
    // The source's own decode gives the digest and length the copy must match, and whether it was clean.
    Result<VerifyReport> source = verifyOutput(job.request.sourcePath, VerifyExpectations{}, job.cancel);
    if (!source.ok()) {
        return source.error();
    }
    job.progress.report(kSourceCheckShare);
    const VerifyReport &original = source.value();
    const audio::DecoderStats &damage = original.stats;

    VerifyExpectations expectations = tagExpectations(job, job.source.container);
    expectations.sampleRate = original.info.sampleRate;
    expectations.channels = original.info.channels;
    expectations.bitsPerSample = original.info.lossless ? original.info.bitsPerSample : 0;
    expectations.expectedFrames = original.decodedFrames;
    expectations.expectedPcmDigest = original.pcmDigest;
    expectations.requireCleanDecode = damage.decodeErrors == 0 && damage.skippedBytes == 0 && !damage.truncated;

    TranscodeReport report;
    report.source = job.source;
    report.outputSampleRate = job.resolved.sampleRate;
    report.outputBitDepth = job.resolved.bitDepth;
    return finishOutput(*temp, job, expectations, std::move(report));
}

} // namespace

namespace detail {

Result<TranscodeReport> transcode(const TranscodeRequest &request, const audio::CancellationFlag *cancel,
                                  const TranscodeProgress &progress, const DeviceWrapper &wrapDevice)
{
    if (const Result<void> valid = validate(request); !valid.ok()) {
        return valid.error();
    }
    if (isCancelled(cancel)) {
        return cancelledError();
    }
    Result<std::unique_ptr<audio::Decoder>> decoder = openSource(request.sourcePath);
    if (!decoder.ok()) {
        return decoder.error();
    }
    ProgressReporter reporter(progress);
    Job job(request, cancel, reporter, wrapDevice);
    job.source = decoder.value()->info();
    job.resolved = resolveOutput(job.source, request.settings);
    job.tags = policyTags(request);
    if (request.settings.format == OutputFormat::KeepAudio) {
        return keepAudio(job);
    }
    return encode(job, decoder.takeValue());
}

} // namespace detail

Result<TranscodeReport> transcode(const TranscodeRequest &request, const audio::CancellationFlag *cancel,
                                  const TranscodeProgress &progress)
{
    return detail::transcode(request, cancel, progress, {});
}

// ---------------------------------------------------------------------------------------------------------------
// TranscodeJob

struct TranscodeJob::Message
{
    enum class Kind { Progress, Finished, Failed };
    Kind kind = Kind::Progress;
    double fraction = 0.0;
    TranscodeReport report;
    Error error;
};

struct TranscodeJob::Shared
{
    std::mutex mutex;
    TranscodeJob *job = nullptr; // cleared by ~TranscodeJob under the mutex
    std::vector<Message> mailbox;
    bool drainQueued = false;
    audio::CancellationFlag cancel;
};

void TranscodeJob::post(const std::shared_ptr<Shared> &shared, Message message)
{
    const std::scoped_lock lock(shared->mutex);
    if (shared->job == nullptr) {
        return;
    }
    shared->mailbox.push_back(std::move(message));
    if (!shared->drainQueued) {
        shared->drainQueued = true;
        // A data-free queued call; Qt drops it if the job is destroyed before it is delivered.
        QMetaObject::invokeMethod(shared->job, "drainMailbox", Qt::QueuedConnection);
    }
}

TranscodeJob::TranscodeJob(TranscodeRequest request, QObject *parent)
    : QObject(parent)
    , m_request(std::move(request))
    , m_shared(std::make_shared<Shared>())
{
    m_shared->job = this;
}

TranscodeJob::~TranscodeJob()
{
    cancel();
    const std::scoped_lock lock(m_shared->mutex);
    m_shared->job = nullptr;
    m_shared->mailbox.clear();
}

void TranscodeJob::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    m_running = true;
    audio::detail::startTask(QThreadPool::globalInstance(), [shared = m_shared, request = m_request] {
        auto lastProgress = std::chrono::steady_clock::now() - kJobProgressInterval;
        const TranscodeProgress progress = [&shared, &lastProgress](double fraction) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastProgress < kJobProgressInterval) {
                return;
            }
            lastProgress = now;
            Message message;
            message.fraction = fraction;
            post(shared, std::move(message));
        };
        Result<TranscodeReport> result = transcode(request, &shared->cancel, progress);
        Message message;
        if (result.ok()) {
            message.kind = Message::Kind::Finished;
            message.report = result.takeValue();
        } else {
            message.kind = Message::Kind::Failed;
            message.error = result.error();
        }
        post(shared, std::move(message));
    });
}

void TranscodeJob::cancel()
{
    m_shared->cancel.cancel();
    m_running = false;
}

void TranscodeJob::drainMailbox()
{
    std::vector<Message> messages;
    {
        const std::scoped_lock lock(m_shared->mutex);
        messages.swap(m_shared->mailbox);
        m_shared->drainQueued = false;
    }
    for (const Message &message : messages) {
        if (!m_running) {
            return; // cancelled: nothing more is emitted
        }
        switch (message.kind) {
        case Message::Kind::Progress:
            Q_EMIT progress(message.fraction);
            break;
        case Message::Kind::Finished:
            m_running = false;
            Q_EMIT finished(message.report);
            break;
        case Message::Kind::Failed:
            m_running = false;
            Q_EMIT failed(message.error);
            break;
        }
    }
}

} // namespace selecta::convert
