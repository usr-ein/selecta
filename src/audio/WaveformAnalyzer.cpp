// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/WaveformAnalyzer.h"

#include "audio/BandAnalyzer.h"
#include "audio/detail/ThreadHandOver.h"

#include <QMetaObject>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::audio {
namespace {

constexpr qint64 kReadFrames = 8192;
constexpr qint64 kStepFrames = qint64{48000} * 30; // one progressive step analyses at most ~30 s of audio

DecoderOptions analysisOptions(const DecoderOptions &base)
{
    DecoderOptions options = base;
    options.channels = 1;
    options.sampleRate = 0;
    return options;
}

// Decodes [start, end) (end < 0: to the end of the stream), warming the filters up on the audio just before
// `start`, and appends the columns (the last chunk also gets its final partial column). Returns the frames analysed.
Result<qint64> analyzeRange(Decoder &decoder, qint64 start, qint64 end, qint64 firstColumn, bool last,
                            QList<WaveformColumn> &columns, const std::function<bool()> &stopped,
                            const std::function<void(qint64)> &onFrames)
{
    const int rate = decoder.sampleRate();
    const qint64 warmFrom = std::max<qint64>(0, start - BandAnalyzer::warmUpFrames(rate));
    if (decoder.seek(warmFrom) != DecodeStatus::Ok) {
        return decoder.error();
    }
    BandAnalyzer analyzer(rate, firstColumn);
    std::vector<float> buffer(kReadFrames);
    qint64 position = warmFrom;
    qint64 analysed = 0;
    while (end < 0 || position < end) {
        if (stopped && stopped()) {
            return makeError(ErrorCode::Cancelled, u"analysis cancelled"_s);
        }
        const qint64 want = end < 0 ? kReadFrames : std::min(kReadFrames, end - position);
        const ReadResult r = decoder.read(buffer.data(), want);
        if (r.status == DecodeStatus::Error) {
            return decoder.error();
        }
        // The part before `start` only warms the filters up.
        const qint64 warm = std::clamp<qint64>(start - position, 0, r.frames);
        if (warm > 0) {
            analyzer.warmUp(std::span<const float>(buffer.data(), static_cast<std::size_t>(warm)));
        }
        const qint64 used = r.frames - warm;
        if (used > 0) {
            analyzer.process(std::span<const float>(buffer.data() + warm, static_cast<std::size_t>(used)), columns);
            analysed += used;
            if (onFrames) {
                onFrames(used);
            }
        }
        position += r.frames;
        if (r.status == DecodeStatus::EndOfStream) {
            break;
        }
        if (r.status == DecodeStatus::NeedMoreData) {
            return makeError(ErrorCode::Internal, u"a complete source asked for more data"_s);
        }
    }
    if (last) {
        analyzer.finish(columns);
    }
    return analysed;
}

// Work shared by the calling thread and the pool tasks of one chunked analysis.
struct ChunkedWork
{
    struct Chunk
    {
        qint64 start = 0;
        qint64 end = -1;
        qint64 firstColumn = 0;
        bool last = false;
        QList<WaveformColumn> columns;
        qint64 frames = 0;
        std::optional<Error> error;
    };

    std::vector<Chunk> chunks;
    std::atomic<std::size_t> next{0};
    std::atomic<qint64> analysed{0};
    qint64 total = -1;
    CancellationFlag failed; // set by the first chunk that fails, so the others stop early
    const CancellationFlag *cancel = nullptr;
    AnalysisProgress progress;
    DecoderOptions decoderOptions;
    std::mutex mutex;
    std::condition_variable allDone;
    std::size_t done = 0;

    // Takes chunks until none is left. Called by the caller and by every pool task.
    void work(const Decoder &prototype)
    {
        std::unique_ptr<Decoder> decoder;
        const auto stopped = [this] { return failed.isCancelled() || (cancel != nullptr && cancel->isCancelled()); };
        const auto onFrames = [this](qint64 frames) {
            const qint64 sofar = analysed.fetch_add(frames) + frames;
            if (progress) {
                progress(sofar, total);
            }
        };
        for (std::size_t k = next.fetch_add(1); k < chunks.size(); k = next.fetch_add(1)) {
            Chunk &chunk = chunks[k];
            if (!decoder) {
                Result<std::unique_ptr<Decoder>> clone = prototype.clone(decoderOptions);
                if (clone.ok()) {
                    decoder = clone.takeValue();
                } else {
                    chunk.error = clone.error();
                }
            }
            if (decoder) {
                const Result<qint64> result = analyzeRange(*decoder, chunk.start, chunk.end, chunk.firstColumn,
                                                           chunk.last, chunk.columns, stopped, onFrames);
                if (result.ok()) {
                    chunk.frames = result.value();
                } else {
                    chunk.error = result.error();
                }
            }
            if (chunk.error) {
                failed.cancel();
            }
            const std::scoped_lock lock(mutex);
            ++done;
            if (done == chunks.size()) {
                allDone.notify_all();
            }
        }
    }

    // The error to report: a real failure rather than the cancellations it caused in other chunks.
    std::optional<Error> firstError() const
    {
        std::optional<Error> cancelled;
        for (const Chunk &chunk : chunks) {
            if (chunk.error && chunk.error->code != ErrorCode::Cancelled) {
                return chunk.error;
            }
            if (chunk.error && !cancelled) {
                cancelled = chunk.error;
            }
        }
        return cancelled;
    }
};

Result<Waveform> analyzeSequentially(Decoder &decoder, const CancellationFlag *cancel, const AnalysisProgress &progress)
{
    QList<WaveformColumn> columns;
    qint64 analysed = 0;
    const qint64 total = decoder.lengthFrames();
    const auto stopped = [cancel] { return cancel != nullptr && cancel->isCancelled(); };
    const Result<qint64> frames = analyzeRange(decoder, 0, -1, 0, true, columns, stopped, [&](qint64 n) {
        analysed += n;
        if (progress) {
            progress(analysed, total);
        }
    });
    if (!frames.ok()) {
        return frames.error();
    }
    return Waveform::fromDetail(decoder.sampleRate(), frames.value(), std::move(columns), true);
}

} // namespace

Result<Waveform> analyzeWaveform(const std::shared_ptr<ByteSource> &source, const AnalysisOptions &options,
                                 const CancellationFlag *cancel, const AnalysisProgress &progress)
{
    if (!source) {
        return makeError(ErrorCode::InvalidArgument, u"no source"_s);
    }
    if (!source->snapshot().isComplete()) {
        return makeError(ErrorCode::NeedsCompleteFile, u"analyzeWaveform() needs a complete source"_s);
    }
    Decoder decoder;
    if (decoder.open(source, analysisOptions(options.decoder)) != Decoder::OpenStatus::Ready) {
        return decoder.error();
    }
    const int rate = decoder.sampleRate();
    const qint64 total = decoder.lengthFrames();
    const int threads = options.maxThreads > 0 ? options.maxThreads : std::max(1, QThread::idealThreadCount());
    const qint64 minChunk = qint64{std::max(1, options.minChunkSeconds)} * rate;
    qint64 chunkCount = 1;
    if (decoder.info().exactSeek && total > 0 && threads > 1) {
        chunkCount = std::clamp<qint64>(total / minChunk, 1, threads);
    }
    if (chunkCount <= 1) {
        return analyzeSequentially(decoder, cancel, progress);
    }

    // Chunks start on column boundaries, so their columns concatenate into exactly the sequential result.
    auto work = std::make_shared<ChunkedWork>();
    work->cancel = cancel;
    work->progress = progress;
    work->total = total;
    work->decoderOptions = analysisOptions(options.decoder);
    const qint64 totalColumns = columnCountForFrames(total, rate);
    work->chunks.resize(static_cast<std::size_t>(chunkCount));
    for (qint64 k = 0; k < chunkCount; ++k) {
        ChunkedWork::Chunk &chunk = work->chunks[static_cast<std::size_t>(k)];
        chunk.firstColumn = (k * totalColumns) / chunkCount;
        chunk.start = columnStartFrame(chunk.firstColumn, rate);
        chunk.last = k == chunkCount - 1;
        chunk.end = chunk.last ? -1 : columnStartFrame(((k + 1) * totalColumns) / chunkCount, rate);
    }
    // The prototype is only cloned (a const operation; the frame index of a complete source is immutable). Tasks that
    // start after the work is done find no chunk left, so they hold their own references.
    const auto prototype = std::make_shared<Decoder>(std::move(decoder));
    QThreadPool *pool = options.pool != nullptr ? options.pool : QThreadPool::globalInstance();
    for (qint64 t = 1; t < chunkCount; ++t) {
        detail::startTask(pool, [work, prototype] { work->work(*prototype); });
    }
    work->work(*prototype); // the caller takes chunks too, so a saturated pool can't deadlock it
    {
        std::unique_lock lock(work->mutex);
        work->allDone.wait(lock, [&work] { return work->done == work->chunks.size(); });
    }
    if (const std::optional<Error> error = work->firstError()) {
        return *error;
    }
    QList<WaveformColumn> columns;
    columns.reserve(totalColumns);
    qint64 frames = 0;
    for (ChunkedWork::Chunk &chunk : work->chunks) {
        columns.append(std::move(chunk.columns));
        frames += chunk.frames;
    }
    return Waveform::fromDetail(rate, frames, std::move(columns), true);
}

// ---------------------------------------------------------------------------------------------------------------
// ProgressiveAnalyzer

struct ProgressiveAnalyzer::Impl
{
    std::shared_ptr<ByteSource> source;
    DecoderOptions options;
    Decoder decoder;
    bool opened = false;
    std::optional<BandAnalyzer> analyzer;
    QList<WaveformColumn> columns;
    qint64 frames = 0;
    bool finished = false;
    Error error;
    std::vector<float> buffer;
};

ProgressiveAnalyzer::ProgressiveAnalyzer(std::shared_ptr<ByteSource> source, const DecoderOptions &decoder)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->source = std::move(source);
    m_impl->options = analysisOptions(decoder);
    m_impl->buffer.resize(kReadFrames);
}

ProgressiveAnalyzer::~ProgressiveAnalyzer() = default;

ProgressiveAnalyzer::Step ProgressiveAnalyzer::step(qint64 maxFrames)
{
    Impl &d = *m_impl;
    Step step;
    step.firstColumn = d.columns.size();
    if (d.finished) {
        step.status = Status::Finished;
        return step;
    }
    if (d.error.isError()) {
        step.status = Status::Failed;
        return step;
    }
    if (!d.opened) {
        if (!d.source) {
            d.error = makeError(ErrorCode::InvalidArgument, u"no source"_s);
            step.status = Status::Failed;
            return step;
        }
        switch (d.decoder.open(d.source, d.options)) {
        case Decoder::OpenStatus::Ready:
            d.opened = true;
            d.analyzer.emplace(d.decoder.sampleRate(), 0);
            break;
        case Decoder::OpenStatus::NeedMoreData:
            step.status = Status::NeedMoreData;
            return step;
        case Decoder::OpenStatus::NeedsCompleteFile:
            step.status = Status::NeedsCompleteFile;
            return step;
        case Decoder::OpenStatus::Failed:
            d.error = d.decoder.error();
            step.status = Status::Failed;
            return step;
        }
    }
    qint64 analysed = 0;
    step.status = Status::Progress;
    if (!d.analyzer) {
        d.error = makeError(ErrorCode::Internal, u"analyzer missing after open"_s);
        step.status = Status::Failed;
        return step;
    }
    BandAnalyzer &analyzer = *d.analyzer;
    while (maxFrames <= 0 || analysed < maxFrames) {
        const ReadResult r = d.decoder.read(d.buffer.data(), kReadFrames);
        if (r.frames > 0) {
            analyzer.process(std::span<const float>(d.buffer.data(), static_cast<std::size_t>(r.frames)), step.columns);
            analysed += r.frames;
            d.frames += r.frames;
        }
        if (r.status == DecodeStatus::EndOfStream) {
            analyzer.finish(step.columns);
            d.finished = true;
            step.status = Status::Finished;
            break;
        }
        if (r.status == DecodeStatus::NeedMoreData) {
            step.status = Status::NeedMoreData;
            break;
        }
        if (r.status == DecodeStatus::Error) {
            d.error = d.decoder.error();
            step.status = Status::Failed;
            break;
        }
    }
    d.columns.append(step.columns);
    return step;
}

Waveform ProgressiveAnalyzer::result() const
{
    const Impl &d = *m_impl;
    if (!d.opened) {
        return {};
    }
    return Waveform::fromDetail(d.decoder.sampleRate(), d.frames, d.columns, d.finished);
}

qint64 ProgressiveAnalyzer::expectedColumns() const
{
    const Impl &d = *m_impl;
    if (!d.opened) {
        return -1;
    }
    const qint64 length = d.decoder.lengthFrames();
    return length >= 0 ? columnCountForFrames(length, d.decoder.sampleRate()) : -1;
}

const StreamInfo &ProgressiveAnalyzer::info() const
{
    return m_impl->decoder.info();
}

const Error &ProgressiveAnalyzer::error() const
{
    return m_impl->error;
}

// ---------------------------------------------------------------------------------------------------------------
// WaveformJob

struct WaveformJob::Message
{
    enum class Kind { ScheduleStep, StepDone, CompleteDone };
    Kind kind = Kind::ScheduleStep;
    ProgressiveAnalyzer::Step step;
    Waveform waveform;
    Error error;
    qint64 expectedColumns = -1;
};

struct WaveformJob::Shared
{
    std::mutex mutex;
    WaveformJob *job = nullptr; // cleared by ~WaveformJob under the mutex
    std::vector<Message> mailbox;
    bool drainQueued = false;
    CancellationFlag cancel;
    std::unique_ptr<ProgressiveAnalyzer> progressive; // used by one step at a time
};

void WaveformJob::post(const std::shared_ptr<Shared> &shared, Message message)
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

void WaveformJob::drainMailbox()
{
    std::vector<Message> messages;
    {
        const std::scoped_lock lock(m_shared->mutex);
        messages.swap(m_shared->mailbox);
        m_shared->drainQueued = false;
    }
    for (const Message &message : messages) {
        switch (message.kind) {
        case Message::Kind::ScheduleStep:
            scheduleStep();
            break;
        case Message::Kind::StepDone:
            onStepDone(message);
            break;
        case Message::Kind::CompleteDone:
            if (message.error.isError()) {
                onCompleteDone(message.error);
            } else {
                onCompleteDone(message.waveform);
            }
            break;
        }
    }
}

WaveformJob::WaveformJob(std::shared_ptr<ByteSource> source, const AnalysisOptions &options, QObject *parent)
    : QObject(parent)
    , m_source(std::move(source))
    , m_options(options)
    , m_shared(std::make_shared<Shared>())
{
    m_shared->job = this;
}

WaveformJob::~WaveformJob()
{
    cancel();
    const std::scoped_lock lock(m_shared->mutex);
    m_shared->job = nullptr;
    m_shared->mailbox.clear();
}

void WaveformJob::setCache(std::shared_ptr<AnalysisCache> cache, const CacheKey &key)
{
    m_cache = std::move(cache);
    m_cacheKey = key;
}

void WaveformJob::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    m_running = true;
    if (!m_source) {
        m_running = false;
        Q_EMIT failed(makeError(ErrorCode::InvalidArgument, u"no source"_s));
        return;
    }
    if (!m_source->snapshot().isComplete()) {
        m_shared->progressive = std::make_unique<ProgressiveAnalyzer>(m_source, m_options.decoder);
        const std::weak_ptr<Shared> weak = m_shared;
        m_subscription = m_source->subscribe([weak] {
            if (const std::shared_ptr<Shared> shared = weak.lock()) {
                post(shared, Message{});
            }
        });
        scheduleStep();
        return;
    }
    QThreadPool *pool = m_options.pool != nullptr ? m_options.pool : QThreadPool::globalInstance();
    detail::startTask(pool,
                      [shared = m_shared, source = m_source, options = m_options, cache = m_cache, key = m_cacheKey] {
                          Message message;
                          message.kind = Message::Kind::CompleteDone;
                          if (cache && !key.isNull()) {
                              const std::optional<Waveform> hit = cache->load(key);
                              if (hit && hit->hasDetail() && hit->isComplete()) {
                                  message.waveform = *hit;
                                  post(shared, std::move(message));
                                  return;
                              }
                          }
                          const Result<Waveform> result = analyzeWaveform(source, options, &shared->cancel);
                          if (result.ok()) {
                              message.waveform = result.value();
                              if (cache && !key.isNull()) {
                                  if (const Result<void> stored = cache->store(key, message.waveform); !stored.ok()) {
                                      qCDebug(lcAudio) << "analysis not cached:" << stored.error().toString();
                                  }
                              }
                          } else {
                              message.error = result.error();
                          }
                          post(shared, std::move(message));
                      });
}

void WaveformJob::cancel()
{
    m_shared->cancel.cancel();
    m_running = false;
    m_subscription.reset();
}

void WaveformJob::scheduleStep()
{
    if (!m_running || m_shared->cancel.isCancelled()) {
        return;
    }
    if (m_stepRunning) {
        m_stepWanted = true;
        return;
    }
    m_stepRunning = true;
    m_stepWanted = false;
    const bool checkCache = !m_checkedCache && m_cache && !m_cacheKey.isNull();
    m_checkedCache = true;
    QThreadPool *pool = m_options.pool != nullptr ? m_options.pool : QThreadPool::globalInstance();
    detail::startTask(pool, [shared = m_shared, cache = m_cache, key = m_cacheKey, checkCache] {
        if (checkCache) {
            const std::optional<Waveform> hit = cache->load(key);
            if (hit && hit->hasDetail() && hit->isComplete()) {
                Message message;
                message.kind = Message::Kind::CompleteDone;
                message.waveform = *hit;
                post(shared, std::move(message));
                return;
            }
        }
        ProgressiveAnalyzer &analyzer = *shared->progressive;
        Message message;
        message.kind = Message::Kind::StepDone;
        message.step = analyzer.step(kStepFrames);
        if (message.step.status == ProgressiveAnalyzer::Status::Finished) {
            message.waveform = analyzer.result();
            if (cache && !key.isNull()) {
                if (const Result<void> stored = cache->store(key, message.waveform); !stored.ok()) {
                    qCDebug(lcAudio) << "analysis not cached:" << stored.error().toString();
                }
            }
        } else if (message.step.status == ProgressiveAnalyzer::Status::Failed) {
            message.error = analyzer.error();
        }
        message.expectedColumns = analyzer.expectedColumns();
        post(shared, std::move(message));
    });
}

void WaveformJob::onStepDone(const Message &message)
{
    m_stepRunning = false;
    if (!m_running || m_shared->cancel.isCancelled()) {
        return;
    }
    const ProgressiveAnalyzer::Step &step = message.step;
    if (!step.columns.isEmpty()) {
        Q_EMIT columnsAdded(step.columns, step.firstColumn, message.expectedColumns);
    }
    switch (step.status) {
    case ProgressiveAnalyzer::Status::Progress:
        scheduleStep();
        return;
    case ProgressiveAnalyzer::Status::NeedMoreData:
        break;
    case ProgressiveAnalyzer::Status::NeedsCompleteFile:
        if (!m_announcedWaiting) {
            m_announcedWaiting = true;
            Q_EMIT waitingForCompleteFile();
        }
        break;
    case ProgressiveAnalyzer::Status::Finished:
        m_running = false;
        m_subscription.reset();
        Q_EMIT finished(message.waveform);
        return;
    case ProgressiveAnalyzer::Status::Failed:
        m_running = false;
        m_subscription.reset();
        Q_EMIT failed(message.error);
        return;
    }
    if (m_stepWanted) {
        scheduleStep(); // the source changed while the step ran
    }
}

void WaveformJob::onCompleteDone(const Result<Waveform> &result)
{
    if (!m_running || m_shared->cancel.isCancelled()) {
        return;
    }
    m_running = false;
    m_subscription.reset();
    if (result.ok()) {
        Q_EMIT finished(result.value());
    } else {
        Q_EMIT failed(result.error());
    }
}

} // namespace selecta::audio
