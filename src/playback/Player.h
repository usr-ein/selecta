// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "playback/AudioOutput.h"

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

class QTimer;

namespace selecta::playback {

class RingBuffer;
class DecodeWorker;

// What is playing, for the player bar.
struct PlayerTrack
{
    QString title;
    QString artist;
    QString source; // where the audio comes from: "Your Inbox", "Preview from vinyl_ghost", "YouTube Music"…
};

// The preview player (PRD PREV-1, PREV-3): plays a file, complete or still downloading, through the audio output.
// A decoder thread fills a lock-free ring that the real-time audio callback empties, so the UI and the network never
// make playback stutter. Seeking past the downloaded part waits for the data; running dry while downloading holds in
// the Buffering state until enough audio is there again, then carries on by itself.
//
// Jumps (play, seek, stop) never wait on another thread: the main thread marks everything in the ring stale at once,
// the decoder marks where the new audio starts, and the audio callback fades out and drops whatever comes before that
// mark. Ring positions only grow, so a mark names one sample for good.
//
// Threading: the API is for the main thread. Decoding runs on the player's own thread, the audio callback on the
// system's audio thread. They share the ring, atomics, and a few mutex-guarded hand-offs the audio thread never
// touches. Only standard primitives cross threads (no queued Qt events), so ThreadSanitizer sees every edge.
class Player : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,    // nothing loaded
        Loading, // opening the file; nothing heard yet
        Playing,
        Paused,
        Buffering, // waiting for the download to catch up
        Ended,     // played to the end; play again with resume() or seek()
        Failed,    // see error()
    };
    Q_ENUM(State)

    explicit Player(std::unique_ptr<AudioOutput> output = AudioOutput::createDefault(), QObject *parent = nullptr);
    ~Player() override;
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;
    Player(Player &&) = delete;
    Player &operator=(Player &&) = delete;

    // Plays `source` from the start, replacing what was playing.
    void play(std::shared_ptr<audio::ByteSource> source, const PlayerTrack &track);
    void pause();
    // Carries on after pause(); after the end, plays again from the start.
    void resume();
    void toggle();
    void stop();
    // Moves to `seconds` (clamped to the length). Past the downloaded part, playback waits for the data. After the
    // end, playback starts again from there.
    void seek(double seconds);
    void skip(double seconds) { seek(position() + seconds); }
    void setVolume(double volume); // 0…1
    double volume() const { return m_volume; }

    State state() const { return m_state; }
    QString error() const { return m_error; }   // why it failed
    QString detail() const { return m_detail; } // why nothing is heard yet, while Loading ("plays once downloaded")
    const PlayerTrack &track() const { return m_track; }
    std::shared_ptr<audio::ByteSource> source() const { return m_source; }
    // Seconds into the track, as heard (the output's latency is accounted for). Right after a seek, the target. Cheap:
    // poll it for the playhead.
    double position() const;
    double duration() const { return m_duration; } // 0 while unknown
    double buffered() const { return m_buffered; } // seconds, from the start, that the bytes there now can play
    bool isActive() const { return m_state != State::Idle && m_state != State::Failed; }

Q_SIGNALS:
    void stateChanged(selecta::playback::Player::State state);
    void detailChanged(const QString &detail);
    void trackChanged(const selecta::playback::PlayerTrack &track);
    void durationChanged(double seconds);
    void bufferedChanged(double seconds);

private:
    friend class DecodeWorker;

    static constexpr std::size_t kOpenEnd = std::numeric_limits<std::size_t>::max();

    // The stretch of audio the decoder is writing: one per play() or seek(). Written by the decoder thread, read by the
    // main thread, under m_segmentMutex.
    struct Segment
    {
        qint64 playId = 0;
        qint64 generation = 0;
        std::size_t start = 0;      // ring position of its first sample
        qint64 firstFrame = 0;      // track frame of its first sample
        std::size_t end = kOpenEnd; // ring position after its last sample, once the decoder reached the end
    };

    // News from the decoder thread, picked up by poll().
    struct DecoderNote
    {
        qint64 playId = 0;
        bool failed = false; // otherwise: why nothing plays yet
        QString message;
    };

    qint64 beginRequest(qint64 frame);
    bool ensureOutput();
    void render(float *destination, int frames); // audio thread
    void poll();                                 // main thread, while active
    void setState(State state);
    void setDetail(const QString &detail);
    void takeDecoderNotes();
    Segment segment() const;
    void publishSegment(const Segment &segment); // decoder thread
    void postNote(DecoderNote note);             // decoder thread

    std::unique_ptr<AudioOutput> m_output;
    int m_sampleRate;
    std::unique_ptr<RingBuffer> m_ring;
    std::size_t m_startHold;    // floats buffered before audio starts after a jump
    std::size_t m_rebufferHold; // floats buffered before audio resumes after running dry
    QTimer *m_poll;
    QTimer *m_idleOutput; // stops the output a while after the end or a pause, so the device can sleep
    std::shared_ptr<audio::ByteSource> m_source;
    PlayerTrack m_track;
    State m_state = State::Idle;
    QString m_error;
    QString m_detail;
    int m_latencyFrames = 0;
    double m_duration = 0.0;
    double m_buffered = 0.0;
    double m_volume = 1.0;
    qint64 m_playId = 0;         // bumped by play() and stop()
    qint64 m_requestedFrame = 0; // where the last play() or seek() asked to go
    bool m_started = false;      // audio of this play() has been heard
    QElapsedTimer m_waitingFor;  // how long the audio callback has been holding for data

    mutable std::mutex m_segmentMutex;
    Segment m_segment;
    std::mutex m_notesMutex;
    std::vector<DecoderNote> m_notes;

    // Shared with the decoder thread and the audio callback.
    std::atomic<qint64> m_generation{0};              // bumped by every jump: the decoder's older work is stale
    std::atomic<std::size_t> m_discardUntil{0};       // the callback drops ring audio before this position
    std::atomic<std::size_t> m_endPosition{kOpenEnd}; // ring position after the last sample, once decoded
    std::atomic<qint64> m_lengthFrames{0};            // 0 while unknown
    std::atomic<qint64> m_decodable{0};               // frames decodable with the bytes there now
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_holding{false};       // the callback waits for enough audio before playing
    std::atomic<bool> m_resetCallback{false}; // set before the output starts: forget the fade and hold
    std::atomic<float> m_gain{1.0F};

    // Audio thread only.
    float m_currentGain = 0.0F; // ramps towards m_gain (or 0 when pausing) so nothing clicks
    bool m_hold = true;
    std::size_t m_holdFloats = 0;

    std::unique_ptr<DecodeWorker> m_worker; // last: its thread uses the members above
};

} // namespace selecta::playback

Q_DECLARE_METATYPE(selecta::playback::PlayerTrack)
