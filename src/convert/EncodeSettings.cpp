// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/EncodeSettings.h"

#include <algorithm>
#include <array>
#include <lame/lame.h>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

constexpr std::array<int, 6> kMp3Bitrates = {128, 160, 192, 224, 256, 320};
constexpr int kMinRate = 8000;
constexpr int kMaxRate = 384000;
constexpr int kMinCeiling = 44100;
constexpr int kLameDefaultQuality = 3; // lame.c's LAME_DEFAULT_QUALITY, what plain `lame -b 320` uses

bool isMpeg1Rate(int rate)
{
    return rate == 32000 || rate == 44100 || rate == 48000;
}

bool isLosslessOutput(OutputFormat format)
{
    return format == OutputFormat::Aiff || format == OutputFormat::Flac || format == OutputFormat::Wav ||
           format == OutputFormat::Alac;
}

// The word length of an integer lossless source, or 0 (lossy, float, unknown).
int integerDepth(const audio::StreamInfo &source)
{
    return source.lossless && !source.floatingPoint ? source.bitsPerSample : 0;
}

// The highest rate of `rate`'s family (44.1 kHz × 2^n, or 48 kHz × 2^n for everything else) that is ≤ ceiling.
int familyRateBelow(int rate, int ceiling)
{
    int base = rate % 11025 == 0 ? 44100 : 48000;
    if (base > ceiling) {
        base = 44100;
    }
    int best = base;
    while (best * 2 <= ceiling) {
        best *= 2;
    }
    return std::min(best, ceiling);
}

QString describeRate(int rate)
{
    return rate % 1000 == 0 ? u"%1 kHz"_s.arg(rate / 1000) : u"%1 kHz"_s.arg(rate / 1000.0, 0, 'f', 1);
}

} // namespace

audio::Result<void> checkSettings(const EncodeSettings &settings)
{
    const auto invalid = [](const QString &why) { return audio::makeError(audio::ErrorCode::InvalidArgument, why); };
    if (settings.format == OutputFormat::Mp3) {
        const Mp3Settings &mp3 = settings.mp3;
        if (mp3.mode == Mp3Mode::Cbr && std::ranges::find(kMp3Bitrates, mp3.bitrateKbps) == kMp3Bitrates.end()) {
            return invalid(u"MP3 bitrate %1 kbps isn't one of 128, 160, 192, 224, 256 or 320"_s.arg(mp3.bitrateKbps));
        }
        if (mp3.mode == Mp3Mode::Vbr && (mp3.vbrQuality < 0 || mp3.vbrQuality > 9)) {
            return invalid(u"MP3 VBR quality V%1 isn't between V0 and V9"_s.arg(mp3.vbrQuality));
        }
        if (mp3.algorithmQuality < 0 || mp3.algorithmQuality > 9) {
            return invalid(u"LAME quality -q %1 isn't between 0 and 9"_s.arg(mp3.algorithmQuality));
        }
        if (settings.sampleRate != 0 && !isMpeg1Rate(settings.sampleRate)) {
            return invalid(u"MP3 output needs 32, 44.1 or 48 kHz, not %1 Hz"_s.arg(settings.sampleRate));
        }
    } else if (settings.sampleRate != 0 && (settings.sampleRate < kMinRate || settings.sampleRate > kMaxRate)) {
        return invalid(u"a sample rate of %1 Hz is outside 8 000…384 000 Hz"_s.arg(settings.sampleRate));
    }
    if (settings.bitDepth != 0 && settings.bitDepth != 16 && settings.bitDepth != 24) {
        return invalid(u"the word length is 16 or 24 bits, not %1"_s.arg(settings.bitDepth));
    }
    if (settings.maxSampleRate < kMinCeiling || settings.maxSampleRate > kMaxRate) {
        return invalid(
            u"the highest sample rate must be between 44 100 and 384 000 Hz, not %1"_s.arg(settings.maxSampleRate));
    }
    return {};
}

ResolvedOutput resolveOutput(const audio::StreamInfo &source, const EncodeSettings &settings)
{
    ResolvedOutput out;
    const int sourceRate = source.sampleRate;
    const int depth = integerDepth(source);
    switch (settings.format) {
    case OutputFormat::KeepAudio:
        out.sampleRate = sourceRate;
        out.bitDepth = depth;
        return out;
    case OutputFormat::Mp3:
        if (settings.sampleRate != 0) {
            out.sampleRate = settings.sampleRate;
        } else {
            out.sampleRate = isMpeg1Rate(sourceRate) ? sourceRate : 44100; // D-4
        }
        break;
    case OutputFormat::Aiff:
    case OutputFormat::Flac:
    case OutputFormat::Wav:
    case OutputFormat::Alac: {
        const int ceiling = std::clamp(settings.maxSampleRate, kMinCeiling, kMaxRate);
        if (settings.sampleRate != 0) {
            out.sampleRate = settings.sampleRate;
        } else {
            out.sampleRate = sourceRate > ceiling ? familyRateBelow(sourceRate, ceiling) : sourceRate;
        }
        if (settings.bitDepth != 0) {
            out.bitDepth = settings.bitDepth;
        } else if (!source.lossless) {
            out.bitDepth = 16; // a decoded lossy stream has no word length; 16-bit holds it
        } else {
            out.bitDepth = depth > 0 && depth <= 16 ? 16 : 24; // 20-, 24-, 32-bit, float and unknown → 24
        }
        out.upDepth = settings.bitDepth != 0 && depth > 0 && settings.bitDepth > depth;
        break;
    }
    }
    out.resample = sourceRate > 0 && out.sampleRate != sourceRate;
    out.upsample = sourceRate > 0 && out.sampleRate > sourceRate;
    if (isLosslessOutput(settings.format) && out.bitDepth == 16) {
        // Only audio already on the 16-bit grid is quantised without dither: an integer source of ≤ 16 bits, played
        // at its own rate, with its own channels.
        const bool onGrid = depth > 0 && depth <= 16 && !out.resample && source.channels <= 2;
        out.dither = !onGrid;
    }
    return out;
}

QString describeSettings(const EncodeSettings &settings)
{
    QString text;
    switch (settings.format) {
    case OutputFormat::Mp3:
        text = settings.mp3.mode == Mp3Mode::Cbr ? u"MP3 %1 kbps CBR"_s.arg(settings.mp3.bitrateKbps)
                                                 : u"MP3 V%1"_s.arg(settings.mp3.vbrQuality);
        break;
    case OutputFormat::Aiff:
        text = u"AIFF"_s;
        break;
    case OutputFormat::Flac:
        text = u"FLAC"_s;
        break;
    case OutputFormat::Wav:
        text = u"WAV"_s;
        break;
    case OutputFormat::Alac:
        text = u"ALAC"_s;
        break;
    case OutputFormat::KeepAudio:
        return u"Original audio"_s;
    }
    if (settings.format != OutputFormat::Mp3 && settings.bitDepth != 0) {
        text += u", %1-bit"_s.arg(settings.bitDepth);
    }
    if (settings.sampleRate != 0) {
        text += u", "_s + describeRate(settings.sampleRate);
    }
    return text;
}

QString fileExtension(OutputFormat format)
{
    switch (format) {
    case OutputFormat::Mp3:
        return u"mp3"_s;
    case OutputFormat::Aiff:
        return u"aiff"_s;
    case OutputFormat::Flac:
        return u"flac"_s;
    case OutputFormat::Wav:
        return u"wav"_s;
    case OutputFormat::Alac:
        return u"m4a"_s;
    case OutputFormat::KeepAudio:
        break;
    }
    return {};
}

QString lameSettingsString(Mp3Settings settings)
{
    QString text = u"LAME"_s + QString::fromLatin1(get_lame_short_version());
    text +=
        settings.mode == Mp3Mode::Cbr ? u" -b %1"_s.arg(settings.bitrateKbps) : u" -V %1"_s.arg(settings.vbrQuality);
    if (settings.algorithmQuality != kLameDefaultQuality) {
        text += u" -q %1"_s.arg(settings.algorithmQuality);
    }
    return text;
}

} // namespace selecta::convert
