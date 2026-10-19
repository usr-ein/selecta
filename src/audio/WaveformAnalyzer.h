// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/AnalysisCache.h"
#include "audio/ByteSource.h"
#include "audio/Decoder.h"
#include "audio/Error.h"
#include "audio/Waveform.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class QThreadPool;

namespace selecta::audio {

struct AnalysisOptions
{
    // Parallel chunks for a complete file: 0 uses QThread::idealThreadCount(); 1 analyses sequentially.
    int maxThreads = 0;
    // Pool for the chunk tasks; nullptr uses QThreadPool::globalInstance(). The calling thread always takes part, so
    // calling from a task of the same pool can't deadlock.
    QThreadPool *pool = nullptr;
    // Smallest chunk worth a task of its own, in seconds of audio.
    int minChunkSeconds = 20;
    DecoderOptions decoder; // channels and sampleRate are overridden: analysis is mono at the source rate
};

// Called from worker threads with (frames analysed so far, total frames or -1). Must be thread-safe and quick.
using AnalysisProgress = std::function<void(qint64 analysedFrames, qint64 totalFrames)>;

// Decodes and analyses a complete source, in parallel chunks when the decoder can seek exactly (MP3, FLAC, WAV, AIFF,
// M4A), sequentially otherwise. Blocks the calling thread until done: call it from a worker or the CLI, never from the
// UI or engine thread (use WaveformJob there). Thread-safe: concurrent calls on different sources are independent.
//
// Failure codes: those of Decoder::open()/read(); NeedsCompleteFile or InvalidArgument when the source isn't
// complete; Cancelled when `cancel` is set (checked between buffers).
Result<Waveform> analyzeWaveform(const std::shared_ptr<ByteSource> &source, const AnalysisOptions &options = {},
                                 const CancellationFlag *cancel = nullptr, const AnalysisProgress &progress = {});

// Progressive analysis of a source that is still downloading (PREV-1/2): each step() decodes and analyses what has
// arrived and returns the new columns. When the source completes, the last step returns Finished and result() is the
// complete waveform, identical to what analyzeWaveform() computes for the finished file.
//
// Threading: not thread-safe; steps must not overlap, but may run on different threads in turn (WaveformJob runs them
// on a thread pool, one at a time).
class ProgressiveAnalyzer
{
public:
    enum class Status {
        Progress,          // new columns; more may be decodable right away (call step() again)
        NeedMoreData,      // wait until the source grows
        NeedsCompleteFile, // this format can't be decoded while downloading; step() again once complete
        Finished,          // the whole file is analysed
        Failed,            // see error()
    };
    struct Step
    {
        Status status = Status::NeedMoreData;
        qint64 firstColumn = 0;        // index of columns.first() in the waveform
        QList<WaveformColumn> columns; // new columns, in order
    };

    explicit ProgressiveAnalyzer(std::shared_ptr<ByteSource> source, const DecoderOptions &decoder = {});
    ~ProgressiveAnalyzer();
    ProgressiveAnalyzer(const ProgressiveAnalyzer &) = delete;
    ProgressiveAnalyzer &operator=(const ProgressiveAnalyzer &) = delete;
    ProgressiveAnalyzer(ProgressiveAnalyzer &&) = delete;
    ProgressiveAnalyzer &operator=(ProgressiveAnalyzer &&) = delete;

    // Analyses at most `maxFrames` more source frames (bounding the time one step takes; ≤ 0: no bound).
    Step step(qint64 maxFrames = 0);

    // Columns analysed so far as a Waveform (isComplete() once Finished).
    Waveform result() const;
    // The expected column count from the stream's announced length, or -1 before open / when unknown.
    qint64 expectedColumns() const;
    const StreamInfo &info() const;
    const Error &error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Asynchronous waveform analysis for the UI and core: runs analyzeWaveform() (complete source) or a
// ProgressiveAnalyzer driven by the source's notifications (growing source) on a thread pool, and reports through
// queued signals.
//
// Threading: lives in the thread that created it, which must run an event loop; every signal is delivered there.
// start() and cancel() must be called from that thread. Destroying the job cancels it; work already running finishes
// in the background without touching the destroyed job.
//
// Cache: with setCache(), a complete cached result (detail included) is reported without decoding, and a finished
// analysis is stored under the key.
class WaveformJob : public QObject
{
    Q_OBJECT

public:
    explicit WaveformJob(std::shared_ptr<ByteSource> source, const AnalysisOptions &options = {},
                         QObject *parent = nullptr);
    ~WaveformJob() override;
    WaveformJob(const WaveformJob &) = delete;
    WaveformJob &operator=(const WaveformJob &) = delete;
    WaveformJob(WaveformJob &&) = delete;
    WaveformJob &operator=(WaveformJob &&) = delete;

    // Optional; call before start(). `cache` must outlive the job.
    void setCache(AnalysisCache *cache, const CacheKey &key);

    // Starts once; later calls do nothing.
    void start();
    // Stops as soon as possible; no further signal is emitted after cancel() returns.
    void cancel();

    bool isRunning() const { return m_running; }

Q_SIGNALS:
    // New columns [firstColumn, firstColumn + columns.size()) while a preview downloads; expectedColumns is -1 when the
    // length is unknown. Not emitted for complete sources, which report finished() directly.
    void columnsAdded(const QList<selecta::audio::WaveformColumn> &columns, qint64 firstColumn,
                      qint64 expectedColumns);
    // The source can't be analysed until the download completes (e.g. M4A with `moov` at the end); the job carries
    // on by itself once it has.
    void waitingForCompleteFile();
    void finished(const selecta::audio::Waveform &waveform);
    void failed(const selecta::audio::Error &error);

private:
    struct Shared;
    void scheduleStep();
    void onStepDone(const ProgressiveAnalyzer::Step &step);
    void onCompleteDone(const Result<Waveform> &result);

    std::shared_ptr<ByteSource> m_source;
    AnalysisOptions m_options;
    std::shared_ptr<Shared> m_shared;
    Subscription m_subscription;
    AnalysisCache *m_cache = nullptr;
    CacheKey m_cacheKey;
    bool m_started = false;
    bool m_running = false;
    bool m_stepRunning = false;
    bool m_stepWanted = false;
    bool m_announcedWaiting = false;
};

} // namespace selecta::audio
