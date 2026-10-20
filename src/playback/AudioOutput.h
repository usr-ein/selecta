// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <functional>
#include <memory>

namespace selecta::playback {

// Where decoded audio goes (ADR-003): the CoreAudio default output on macOS, a silent stand-in in tests and headless
// runs. Interleaved float stereo.
//
// The render function runs on the system's real-time audio thread: it must not lock, allocate or block. It fills
// `frames` frames of `destination` (frames × 2 floats).
class AudioOutput
{
public:
    using Render = std::function<void(float *destination, int frames)>;

    AudioOutput() = default;
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;
    AudioOutput(AudioOutput &&) = delete;
    AudioOutput &operator=(AudioOutput &&) = delete;
    virtual ~AudioOutput() = default;

    // The rate the output runs at; decode to it to avoid a second conversion. 0 when there is no output device.
    virtual int sampleRate() const = 0;
    // Starts pulling from `render`. Returns false, with error() set, when the output can't run.
    virtual bool start(Render render) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    // Frames between a sample leaving render() and reaching the speakers (device, safety offset, buffer).
    virtual int latencyFrames() const = 0;
    virtual QString error() const = 0;

    // The system's default output (CoreAudio on macOS), or a silent output where there is none.
    static std::unique_ptr<AudioOutput> createDefault();
    // An output that pulls in real time from a timer and throws the audio away: tests and headless runs.
    static std::unique_ptr<AudioOutput> createSilent(int sampleRate = 48'000);
};

} // namespace selecta::playback
