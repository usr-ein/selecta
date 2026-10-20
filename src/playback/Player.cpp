// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback/Player.h"

#include "audio/Decoder.h"
#include "playback/RingBuffer.h"

#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::playback {
namespace {

constexpr int kChannels = 2;
constexpr int kFallbackRate = 48'000;
constexpr double kRingSeconds = 0.75;       // decoded audio kept ahead of the speakers (rounded up to a power of two)
constexpr double kStartHoldSeconds = 0.1;   // after a jump: short, so seeking feels instant
constexpr double kRebufferSeconds = 0.5;    // after running dry: long enough not to stutter on a slow download
constexpr int kFillIntervalMs = 20;         // the decoder tops the ring up this often
constexpr int kChunkFrames = 2048;          // frames decoded per read
constexpr int kPollIntervalMs = 33;         // state and playhead updates on the main thread
constexpr int kBufferingAfterMs = 150;      // shorter holds (every seek has one) don't show as Buffering
constexpr int kOutputIdleAfterEndMs = 1000; // the output keeps running briefly in case playback goes on
constexpr int kOutputIdleAfterPauseMs = 15'000;
constexpr int kFadeFrames = 256; // ~5 ms: fades on play, pause, resume and seek so nothing clicks

std::size_t floatsFor(double seconds, int sampleRate)
{
    return static_cast<std::size_t>(std::ceil(seconds * sampleRate)) * kChannels;
}

// Ramps `gain` towards `target` by `step` per frame over `frames` interleaved stereo frames.
void applyGain(float *samples, std::size_t frames, float &gain, float target, float step)
{
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (gain < target) {
            gain = std::min(target, gain + step);
        } else if (gain > target) {
            gain = std::max(target, gain - step);
        }
        samples[frame * kChannels] *= gain;
        samples[(frame * kChannels) + 1] *= gain;
    }
}

} // namespace

// Owns the Decoder on the player's thread and keeps the ring topped up. Every request carries the generation the main
// thread gave it; work for an older generation is dropped, since a newer request is already queued behind it.
// A QObject only for its thread affinity and timer: no signals, slots or properties.
class DecodeWorker : public QObject // clazy:exclude=missing-qobject-macro
{
public:
    DecodeWorker(Player &player, RingBuffer &ring)
        : m_player(player)
        , m_ring(ring)
        , m_timer(new QTimer(this))
        , m_buffer(static_cast<std::size_t>(kChunkFrames) * kChannels)
    {
        m_timer->setInterval(kFillIntervalMs);
        connect(m_timer, &QTimer::timeout, this, &DecodeWorker::fill);
    }

    void open(const std::shared_ptr<audio::ByteSource> &source, int sampleRate, qint64 playId, qint64 generation)
    {
        m_decoder.close();
        m_source = source;
        m_sampleRate = sampleRate;
        m_playId = playId;
        m_opened = false;
        m_failed = false;
        m_atEnd = false;
        m_waitForComplete = false;
        m_pendingSeek.reset();
        // Reset before the segment is published: once the main thread sees this play's segment, these are its own.
        m_player.m_lengthFrames.store(0, std::memory_order_relaxed);
        m_player.m_decodable.store(0, std::memory_order_relaxed);
        beginSegment(generation, 0);
        m_timer->start();
        fill();
    }

    void seek(qint64 frame, qint64 generation)
    {
        if (!m_source || m_failed || generation != m_player.m_generation.load(std::memory_order_acquire)) {
            return; // a newer jump is queued behind this one
        }
        m_atEnd = false;
        m_pendingSeek = frame;
        beginSegment(generation, frame);
        m_timer->start();
        fill();
    }

    void close()
    {
        m_timer->stop();
        m_decoder.close();
        m_source.reset();
        m_opened = false;
        m_pendingSeek.reset();
    }

private:
    // Starts a new stretch at the ring's write position. The end mark is cleared before the discard mark moves, so the
    // callback never pairs the new discard mark with the previous stretch's end.
    void beginSegment(qint64 generation, qint64 firstFrame)
    {
        m_generation = generation;
        m_segment = {m_playId, generation, m_ring.writePosition(), firstFrame, Player::kOpenEnd};
        m_player.m_endPosition.store(Player::kOpenEnd, std::memory_order_release);
        m_player.publishSegment(m_segment);
        m_player.m_discardUntil.store(m_segment.start, std::memory_order_release);
    }

    bool isCurrent() const { return m_generation == m_player.m_generation.load(std::memory_order_acquire); }

    void fill()
    {
        if (!m_source || m_failed || m_atEnd || !isCurrent()) {
            return;
        }
        if (!m_opened && !openDecoder()) {
            return;
        }
        if (m_pendingSeek && !applySeek(*m_pendingSeek)) {
            return;
        }
        m_player.m_decodable.store(m_decoder.decodableFrames(), std::memory_order_relaxed);
        while (m_ring.freeSpace() >= m_buffer.size() && isCurrent()) {
            const audio::ReadResult result = m_decoder.read(m_buffer.data(), kChunkFrames);
            if (result.frames > 0) {
                m_ring.write(m_buffer.data(), static_cast<std::size_t>(result.frames) * kChannels);
            }
            switch (result.status) {
            case audio::DecodeStatus::Ok:
                break;
            case audio::DecodeStatus::NeedMoreData:
                return; // the download hasn't caught up: try again on the next tick
            case audio::DecodeStatus::EndOfStream:
                reachedEnd();
                return;
            case audio::DecodeStatus::Error:
                fail(m_decoder.error().message);
                return;
            }
        }
    }

    bool openDecoder()
    {
        if (m_waitForComplete && !m_source->snapshot().isComplete()) {
            return false;
        }
        audio::DecoderOptions options;
        options.sampleRate = m_sampleRate;
        options.channels = kChannels;
        switch (m_decoder.open(m_source, options)) {
        case audio::Decoder::OpenStatus::Ready:
            m_opened = true;
            m_player.m_lengthFrames.store(std::max<qint64>(0, m_decoder.lengthFrames()), std::memory_order_relaxed);
            return true;
        case audio::Decoder::OpenStatus::NeedMoreData:
            return false;
        case audio::Decoder::OpenStatus::NeedsCompleteFile:
            if (!m_waitForComplete) {
                m_waitForComplete = true;
                tellPlayer(&Player::decoderWaiting, Player::tr("This file plays once it has downloaded."));
            }
            return false;
        case audio::Decoder::OpenStatus::Failed:
            fail(m_decoder.error().message);
            return false;
        }
        return false;
    }

    bool applySeek(qint64 frame)
    {
        switch (m_decoder.seek(frame)) {
        case audio::DecodeStatus::Ok:
        case audio::DecodeStatus::EndOfStream:
            break;
        case audio::DecodeStatus::NeedMoreData:
            return false; // the target hasn't downloaded yet
        case audio::DecodeStatus::Error:
            fail(m_decoder.error().message);
            return false;
        }
        m_pendingSeek.reset();
        // Where decoding really resumes: formats without exact seeking land on the nearest frame they can reach.
        const qint64 landed = m_decoder.position();
        if (landed >= 0 && landed != m_segment.firstFrame) {
            m_segment.firstFrame = landed;
            m_player.publishSegment(m_segment);
        }
        return true;
    }

    void reachedEnd()
    {
        m_atEnd = true;
        m_timer->stop();
        m_segment.end = m_ring.writePosition();
        m_player.publishSegment(m_segment);
        m_player.m_endPosition.store(m_segment.end, std::memory_order_release);
        if (m_player.m_lengthFrames.load(std::memory_order_relaxed) <= 0) {
            m_player.m_lengthFrames.store(std::max<qint64>(0, m_decoder.position()), std::memory_order_relaxed);
        }
        m_player.m_decodable.store(m_player.m_lengthFrames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void fail(const QString &message)
    {
        m_failed = true;
        m_timer->stop();
        m_decoder.close();
        m_opened = false;
        tellPlayer(&Player::decoderFailed, message.isEmpty() ? Player::tr("This file can't be played.") : message);
    }

    // Runs `method` on the player's thread, tagged with this play's id so news about a replaced track is dropped.
    void tellPlayer(void (Player::*method)(qint64, const QString &), const QString &message)
    {
        QMetaObject::invokeMethod(
            &m_player, [player = &m_player, method, playId = m_playId, message] { (player->*method)(playId, message); },
            Qt::QueuedConnection);
    }

    Player &m_player;
    RingBuffer &m_ring;
    QTimer *m_timer;
    audio::Decoder m_decoder;
    std::shared_ptr<audio::ByteSource> m_source;
    std::vector<float> m_buffer;
    std::optional<qint64> m_pendingSeek;
    Player::Segment m_segment;
    qint64 m_playId = 0;
    qint64 m_generation = 0;
    int m_sampleRate = 0;
    bool m_opened = false;
    bool m_failed = false;
    bool m_atEnd = false;
    bool m_waitForComplete = false;
};

Player::Player(std::unique_ptr<AudioOutput> output, QObject *parent)
    : QObject(parent)
    , m_output(std::move(output))
    , m_sampleRate(m_output->sampleRate() > 0 ? m_output->sampleRate() : kFallbackRate)
    , m_ring(std::make_unique<RingBuffer>(floatsFor(kRingSeconds, m_sampleRate)))
    , m_startHold(std::min(floatsFor(kStartHoldSeconds, m_sampleRate), m_ring->capacity() / 2))
    , m_rebufferHold(std::min(floatsFor(kRebufferSeconds, m_sampleRate), m_ring->capacity() / 2))
    , m_worker(new DecodeWorker(*this, *m_ring))
    , m_poll(new QTimer(this))
    , m_idleOutput(new QTimer(this))
{
    qRegisterMetaType<PlayerTrack>();
    m_thread.setObjectName(u"selecta-decoder"_s);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();

    m_poll->setInterval(kPollIntervalMs);
    connect(m_poll, &QTimer::timeout, this, &Player::poll);
    m_idleOutput->setSingleShot(true);
    connect(m_idleOutput, &QTimer::timeout, this, [this] {
        if (m_state == State::Ended || m_state == State::Paused) {
            m_output->stop();
        }
    });
}

Player::~Player()
{
    m_output->stop(); // the callback reads members: stop it first
    QMetaObject::invokeMethod(m_worker, &DecodeWorker::close, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait(); // deletes the worker (QThread::finished → deleteLater)
}

qint64 Player::beginRequest(qint64 frame)
{
    const qint64 generation = m_generation.load(std::memory_order_relaxed) + 1;
    m_generation.store(generation, std::memory_order_release);
    // Everything in the ring is stale from now on: the callback fades it out and drops it, and keeps dropping until
    // the decoder marks where the new audio starts.
    m_discardUntil.store(kOpenEnd, std::memory_order_release);
    m_requestedFrame = frame;
    m_waitingFor.invalidate();
    m_idleOutput->stop();
    return generation;
}

bool Player::ensureOutput()
{
    if (m_output->isRunning()) {
        return true;
    }
    m_resetCallback.store(true, std::memory_order_release);
    if (!m_output->start([this](float *destination, int frames) { render(destination, frames); })) {
        m_error = tr("No sound output: %1").arg(m_output->error());
        setState(State::Failed);
        return false;
    }
    m_latencyFrames = std::max(0, m_output->latencyFrames());
    return true;
}

void Player::play(std::shared_ptr<audio::ByteSource> source, const PlayerTrack &track)
{
    if (!source) {
        return;
    }
    const qint64 generation = beginRequest(0);
    const qint64 playId = ++m_playId;
    m_source = std::move(source);
    m_track = track;
    m_error.clear();
    m_started = false;
    m_duration = 0.0;
    m_buffered = 0.0;
    m_paused.store(false, std::memory_order_relaxed);
    Q_EMIT trackChanged(m_track);
    Q_EMIT durationChanged(0.0);
    Q_EMIT bufferedChanged(0.0);
    setDetail({});
    setState(State::Loading);
    if (!ensureOutput()) {
        return;
    }
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, source = m_source, rate = m_sampleRate, playId, generation] {
            worker->open(source, rate, playId, generation);
        },
        Qt::QueuedConnection);
    m_poll->start();
}

void Player::pause()
{
    if (m_state == State::Playing || m_state == State::Buffering || m_state == State::Loading) {
        m_paused.store(true, std::memory_order_relaxed);
        setState(State::Paused);
        m_idleOutput->start(kOutputIdleAfterPauseMs);
    }
}

void Player::resume()
{
    if (m_state == State::Paused) {
        m_idleOutput->stop();
        m_paused.store(false, std::memory_order_relaxed);
        if (!ensureOutput()) {
            return;
        }
        m_waitingFor.invalidate();
        setState(m_started ? State::Playing : State::Loading);
        m_poll->start();
    } else if (m_state == State::Ended) {
        seek(0.0);
    }
}

void Player::toggle()
{
    switch (m_state) {
    case State::Paused:
    case State::Ended:
        resume();
        break;
    case State::Loading:
    case State::Playing:
    case State::Buffering:
        pause();
        break;
    case State::Idle:
    case State::Failed:
        break;
    }
}

void Player::stop()
{
    beginRequest(0);
    ++m_playId;
    QMetaObject::invokeMethod(m_worker, &DecodeWorker::close, Qt::QueuedConnection);
    m_output->stop();
    m_poll->stop();
    m_source.reset();
    m_track = {};
    m_error.clear();
    m_started = false;
    m_duration = 0.0;
    m_buffered = 0.0;
    m_paused.store(false, std::memory_order_relaxed);
    Q_EMIT trackChanged(m_track);
    setDetail({});
    setState(State::Idle);
}

void Player::seek(double seconds)
{
    if (!m_source || !isActive()) {
        return;
    }
    qint64 frame = std::llround(std::max(0.0, seconds) * m_sampleRate);
    if (const qint64 length = m_lengthFrames.load(std::memory_order_relaxed); length > 0) {
        frame = std::min(frame, length);
    }
    const qint64 generation = beginRequest(frame);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, frame, generation] { worker->seek(frame, generation); }, Qt::QueuedConnection);
    if (m_state == State::Ended) {
        m_paused.store(false, std::memory_order_relaxed);
        if (!ensureOutput()) {
            return;
        }
        setState(State::Playing);
        m_poll->start();
    }
}

void Player::setVolume(double volume)
{
    m_volume = std::clamp(volume, 0.0, 1.0);
    // Squared: loudness follows the slider more evenly than with a linear gain.
    m_gain.store(static_cast<float>(m_volume * m_volume), std::memory_order_relaxed);
}

double Player::position() const
{
    if (!m_source || !isActive()) {
        return 0.0;
    }
    const qint64 length = m_lengthFrames.load(std::memory_order_relaxed);
    if (m_state == State::Ended && length > 0) {
        return static_cast<double>(length) / m_sampleRate;
    }
    const Segment current = segment();
    qint64 frame = m_requestedFrame;
    if (current.playId == m_playId && current.generation == m_generation.load(std::memory_order_relaxed)) {
        const std::size_t read = m_ring->readPosition();
        const qint64 consumed = read > current.start ? static_cast<qint64>((read - current.start) / kChannels) : 0;
        frame = current.firstFrame + std::max<qint64>(0, consumed - m_latencyFrames);
    }
    if (length > 0) {
        frame = std::min(frame, length);
    }
    return static_cast<double>(frame) / m_sampleRate;
}

void Player::render(float *destination, int frames)
{
    const std::size_t wanted = static_cast<std::size_t>(std::max(0, frames)) * kChannels;
    constexpr float kStep = 1.0F / kFadeFrames;
    if (m_resetCallback.exchange(false, std::memory_order_acquire)) {
        m_currentGain = 0.0F;
        m_hold = true;
        m_holdFloats = m_startHold;
    }
    std::size_t done = 0;

    // A jump: fade out what was playing, then drop everything before the decoder's mark.
    const std::size_t discardUntil = m_discardUntil.load(std::memory_order_acquire);
    if (m_ring->readPosition() < discardUntil) {
        if (m_currentGain > 0.0F && !m_hold) {
            const std::size_t stale = std::min(discardUntil - m_ring->readPosition(), m_ring->available());
            done =
                m_ring->read(destination, std::min({stale, wanted, static_cast<std::size_t>(kFadeFrames) * kChannels}));
            const std::size_t fadeFrames = done / kChannels;
            const float step = fadeFrames > 0 ? m_currentGain / static_cast<float>(fadeFrames) : 1.0F;
            applyGain(destination, fadeFrames, m_currentGain, 0.0F, step);
        }
        m_ring->skipTo(discardUntil);
        m_currentGain = 0.0F;
        m_hold = true;
        m_holdFloats = m_startHold;
    }

    // Hold until enough audio is there, unless the rest of the track already is.
    const std::size_t end = m_endPosition.load(std::memory_order_acquire);
    const bool complete = end != kOpenEnd && end >= m_ring->readPosition();
    if (m_hold && (m_ring->available() >= m_holdFloats || complete)) {
        m_hold = false;
    }
    m_holding.store(m_hold, std::memory_order_relaxed);

    const bool paused = m_paused.load(std::memory_order_relaxed);
    if (!m_hold && (!paused || m_currentGain > 0.0F)) {
        std::size_t want = wanted - done;
        if (paused) {
            // Only what the fade-out needs: the rest stays in the ring for resume().
            const auto fade = static_cast<std::size_t>(std::ceil(m_currentGain / kStep));
            want = std::min(want, fade * kChannels);
        }
        const std::size_t got = m_ring->read(destination + done, want);
        applyGain(destination + done, got / kChannels, m_currentGain,
                  paused ? 0.0F : m_gain.load(std::memory_order_relaxed), kStep);
        done += got;
        if (!paused && got < want && !complete) {
            // Ran dry before the end: the download is behind. Wait for a comfortable margin before going on.
            m_hold = true;
            m_holdFloats = m_rebufferHold;
            m_currentGain = 0.0F;
            m_holding.store(true, std::memory_order_relaxed);
        }
    }
    std::fill(destination + done, destination + wanted, 0.0F);
}

void Player::poll()
{
    const Segment current = segment();
    const bool ownSegment = current.playId == m_playId;
    if (ownSegment) {
        // Before this play's segment shows up, the decoder's figures may still be the previous track's.
        const qint64 length = m_lengthFrames.load(std::memory_order_relaxed);
        const double duration = static_cast<double>(length) / m_sampleRate;
        if (std::abs(duration - m_duration) > 0.001) {
            m_duration = duration;
            Q_EMIT durationChanged(m_duration);
        }
        const double buffered =
            std::min(static_cast<double>(m_decodable.load(std::memory_order_relaxed)) / m_sampleRate,
                     m_duration > 0.0 ? m_duration : std::numeric_limits<double>::max());
        if (std::abs(buffered - m_buffered) >= 0.1 || (buffered != m_buffered && buffered >= m_duration)) {
            m_buffered = buffered;
            Q_EMIT bufferedChanged(m_buffered);
        }
    }
    if (m_state != State::Loading && m_state != State::Playing && m_state != State::Buffering) {
        return;
    }
    const bool caughtUp = ownSegment && current.generation == m_generation.load(std::memory_order_relaxed);
    const std::size_t read = m_ring->readPosition();
    if (caughtUp && read > current.start && !m_started) {
        m_started = true;
        setDetail({});
    }
    if (caughtUp && current.end != kOpenEnd && read >= current.end) {
        setState(State::Ended);
        m_poll->stop();
        // The last samples are still in the device's buffer: give them time before stopping the output.
        m_idleOutput->start(kOutputIdleAfterEndMs);
        return;
    }
    if (caughtUp && !m_holding.load(std::memory_order_relaxed)) {
        m_waitingFor.invalidate();
        setState(m_started ? State::Playing : State::Loading);
        return;
    }
    if (!m_waitingFor.isValid()) {
        m_waitingFor.start();
    }
    if (m_started && m_waitingFor.elapsed() >= kBufferingAfterMs) {
        setState(State::Buffering);
    }
}

void Player::setState(State state)
{
    if (state == m_state) {
        return;
    }
    m_state = state;
    if (state == State::Failed || state == State::Idle) {
        m_poll->stop();
        m_idleOutput->stop();
    }
    Q_EMIT stateChanged(state);
}

void Player::setDetail(const QString &detail)
{
    if (detail != m_detail) {
        m_detail = detail;
        Q_EMIT detailChanged(m_detail);
    }
}

void Player::decoderFailed(qint64 playId, const QString &message)
{
    if (playId != m_playId || !isActive()) {
        return; // about a track that has since been replaced
    }
    m_error = message;
    m_output->stop();
    setDetail({});
    setState(State::Failed);
}

void Player::decoderWaiting(qint64 playId, const QString &message)
{
    if (playId == m_playId && !m_started) {
        setDetail(message);
    }
}

Player::Segment Player::segment() const
{
    const QMutexLocker lock(&m_segmentMutex);
    return m_segment;
}

void Player::publishSegment(const Segment &segment)
{
    const QMutexLocker lock(&m_segmentMutex);
    m_segment = segment;
}

} // namespace selecta::playback
