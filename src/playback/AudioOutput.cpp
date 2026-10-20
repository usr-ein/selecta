// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback/AudioOutput.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QTimer>

#include <vector>

#ifdef Q_OS_MACOS
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::playback {
namespace {

Q_LOGGING_CATEGORY(lcOutput, "selecta.playback.output")

constexpr int kChannels = 2;

// Pulls in real time from a timer, on the thread that owns it, and drops the audio.
class SilentOutput final : public AudioOutput
{
public:
    explicit SilentOutput(int sampleRate)
        : m_sampleRate(sampleRate)
    {
        m_timer.setInterval(10);
        m_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { pull(); });
    }

    int sampleRate() const override { return m_sampleRate; }
    bool start(Render render) override
    {
        m_render = std::move(render);
        m_clock.start();
        m_pulled = 0;
        m_timer.start();
        return true;
    }
    void stop() override
    {
        m_timer.stop();
        m_render = {};
    }
    bool isRunning() const override { return m_timer.isActive(); }
    int latencyFrames() const override { return 0; }
    QString error() const override { return {}; }

private:
    void pull()
    {
        // As many frames as real time says are due, so positions advance at the true rate.
        const qint64 due = m_clock.elapsed() * m_sampleRate / 1000;
        const auto frames = static_cast<int>(std::min<qint64>(due - m_pulled, m_sampleRate));
        if (frames <= 0 || !m_render) {
            return;
        }
        m_buffer.resize(static_cast<std::size_t>(frames) * kChannels);
        m_render(m_buffer.data(), frames);
        m_pulled += frames;
    }

    int m_sampleRate;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_pulled = 0;
    std::vector<float> m_buffer;
    Render m_render;
};

#ifdef Q_OS_MACOS

QString statusText(OSStatus status)
{
    return u"OSStatus %1"_s.arg(status);
}

template <typename T>
bool deviceProperty(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope,
                    T *value)
{
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, value) == noErr;
}

// The default output through an AUHAL output unit (ADR-003): interleaved float stereo at the device's rate.
class CoreAudioOutput final : public AudioOutput
{
public:
    CoreAudioOutput()
    {
        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_DefaultOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (!component) {
            m_error = u"No audio output component"_s;
            return;
        }
        if (const OSStatus status = AudioComponentInstanceNew(component, &m_unit); status != noErr) {
            m_error = u"Can't open the audio output: "_s + statusText(status);
            m_unit = nullptr;
            return;
        }
        AudioStreamBasicDescription device{};
        UInt32 size = sizeof(device);
        if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size) ==
                noErr &&
            device.mSampleRate > 0) {
            m_sampleRate = static_cast<int>(device.mSampleRate);
        } else {
            m_sampleRate = 48'000;
        }
        AudioStreamBasicDescription format{};
        format.mSampleRate = m_sampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mBitsPerChannel = 32;
        format.mChannelsPerFrame = kChannels;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = sizeof(float) * kChannels;
        format.mBytesPerPacket = format.mBytesPerFrame;
        if (const OSStatus status = AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                                         0, &format, sizeof(format));
            status != noErr) {
            m_error = u"The audio output refused the format: "_s + statusText(status);
            return;
        }
        AURenderCallbackStruct callback{&CoreAudioOutput::renderCallback, this};
        if (const OSStatus status = AudioUnitSetProperty(m_unit, kAudioUnitProperty_SetRenderCallback,
                                                         kAudioUnitScope_Input, 0, &callback, sizeof(callback));
            status != noErr) {
            m_error = u"Can't feed the audio output: "_s + statusText(status);
            return;
        }
        if (const OSStatus status = AudioUnitInitialize(m_unit); status != noErr) {
            m_error = u"Can't start the audio output: "_s + statusText(status);
            return;
        }
        m_initialized = true;
    }

    ~CoreAudioOutput() override
    {
        stop();
        if (m_unit) {
            if (m_initialized) {
                AudioUnitUninitialize(m_unit);
            }
            AudioComponentInstanceDispose(m_unit);
        }
    }
    CoreAudioOutput(const CoreAudioOutput &) = delete;
    CoreAudioOutput &operator=(const CoreAudioOutput &) = delete;
    CoreAudioOutput(CoreAudioOutput &&) = delete;
    CoreAudioOutput &operator=(CoreAudioOutput &&) = delete;

    int sampleRate() const override { return m_initialized ? m_sampleRate : 0; }

    bool start(Render render) override
    {
        if (!m_initialized) {
            return false;
        }
        if (m_running) {
            stop();
        }
        m_render = std::move(render); // set before the unit starts calling it
        if (const OSStatus status = AudioOutputUnitStart(m_unit); status != noErr) {
            m_error = u"Can't start the audio output: "_s + statusText(status);
            m_render = {};
            return false;
        }
        m_running = true;
        return true;
    }

    void stop() override
    {
        if (m_running) {
            AudioOutputUnitStop(m_unit); // returns once the render callback has finished
            m_running = false;
        }
        m_render = {};
    }

    bool isRunning() const override { return m_running; }

    int latencyFrames() const override
    {
        AudioObjectID device = kAudioObjectUnknown;
        if (!deviceProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                            kAudioObjectPropertyScopeGlobal, &device) ||
            device == kAudioObjectUnknown) {
            return 0;
        }
        UInt32 latency = 0;
        UInt32 safety = 0;
        UInt32 buffer = 0;
        deviceProperty(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &latency);
        deviceProperty(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &safety);
        deviceProperty(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &buffer);
        return static_cast<int>(latency + safety + buffer);
    }

    QString error() const override { return m_error; }

private:
    static OSStatus renderCallback(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                                   UInt32 bus, UInt32 frames, AudioBufferList *data)
    {
        Q_UNUSED(timestamp)
        Q_UNUSED(bus)
        const auto *self = static_cast<const CoreAudioOutput *>(context);
        if (data == nullptr || data->mNumberBuffers < 1 || data->mBuffers[0].mData == nullptr) {
            return noErr;
        }
        auto *destination = static_cast<float *>(data->mBuffers[0].mData);
        const UInt32 capacity = data->mBuffers[0].mDataByteSize / (sizeof(float) * kChannels);
        const UInt32 count = std::min(frames, capacity);
        if (self->m_render) {
            self->m_render(destination, static_cast<int>(count));
        } else {
            std::fill_n(destination, static_cast<std::size_t>(count) * kChannels, 0.0F);
            if (flags) {
                *flags |= kAudioUnitRenderAction_OutputIsSilence;
            }
        }
        return noErr;
    }

    AudioComponentInstance m_unit = nullptr;
    int m_sampleRate = 0;
    bool m_initialized = false;
    bool m_running = false;
    Render m_render;
    QString m_error;
};

#endif

} // namespace

std::unique_ptr<AudioOutput> AudioOutput::createDefault()
{
#ifdef Q_OS_MACOS
    auto output = std::make_unique<CoreAudioOutput>();
    if (output->sampleRate() > 0) {
        return output;
    }
    qCWarning(lcOutput).noquote() << "no audio output:" << output->error() << "- previews will be silent";
#endif
    return createSilent();
}

std::unique_ptr<AudioOutput> AudioOutput::createSilent(int sampleRate)
{
    return std::make_unique<SilentOutput>(sampleRate);
}

} // namespace selecta::playback
