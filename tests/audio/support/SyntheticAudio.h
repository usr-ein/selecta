// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <initializer_list>
#include <vector>

// Synthetic test audio, generated at test time: tones, sweeps, noise, and files in every format built from them with
// LAME and FFmpeg's own encoders. No recorded or copyrighted audio is ever used (CLAUDE.md, contract).
namespace selecta::testing {

struct Pcm
{
    int sampleRate = 44100;
    int channels = 2;
    std::vector<float> samples; // interleaved

    qint64 frames() const { return channels > 0 ? static_cast<qint64>(samples.size()) / channels : 0; }
    float at(qint64 frame, int channel) const
    {
        return samples[static_cast<std::size_t>((frame * channels) + channel)];
    }
};

// Every channel carries the same signal unless stated otherwise.
Pcm sine(double frequency, double seconds, int sampleRate = 44100, int channels = 2, double amplitude = 0.5);
Pcm tones(std::initializer_list<double> frequencies, double seconds, int sampleRate = 44100, int channels = 2,
          double amplitude = 0.25);
Pcm logSweep(double from, double to, double seconds, int sampleRate = 44100, int channels = 2, double amplitude = 0.5);
Pcm whiteNoise(double seconds, unsigned seed, int sampleRate = 44100, int channels = 2, double amplitude = 0.3);
// White noise with nothing at or above `cutoffHz`, an exact low-pass like a lossy encoder's: random phases (from
// `seed`) on every bin below it of a 2^17-point inverse real FFT, repeated to `seconds` and scaled to `rms`.
Pcm brickwallNoise(double cutoffHz, double seconds, unsigned seed, int sampleRate = 44100, int channels = 2,
                   double rms = 0.1);
Pcm silence(double seconds, int sampleRate = 44100, int channels = 2);
// A different sine on each channel (left `left` Hz, right `right` Hz), to check channel order and mixing.
Pcm stereoPair(double left, double right, double seconds, int sampleRate = 44100, double amplitude = 0.4);
// Rounds every sample to `bits`-bit integer PCM, so lossless round trips can be compared exactly.
Pcm quantised(const Pcm &pcm, int bits);
Pcm concatenated(const Pcm &a, const Pcm &b);

struct WavOptions
{
    int bits = 16; // 8, 16, 24, 32 (integer) or 32/64 with floatingPoint
    bool floatingPoint = false;
    bool extensible = false;    // WAVE_FORMAT_EXTENSIBLE
    int listChunkBytes = 0;     // a LIST chunk of this size before `data`
    int trailingChunkBytes = 0; // an `id3 ` chunk after `data`
    bool dataBeforeFormat = false;
    bool unknownDataSize = false; // data size 0xFFFFFFFF, as streamed WAVs write it
};
QByteArray makeWav(const Pcm &pcm, const WavOptions &options = {});

struct AiffOptions
{
    int bits = 16;
    bool commAfterSsnd = false;
    QByteArray compression; // empty: plain AIFF; "sowt", "fl32", "NONE": AIFF-C
    int id3ChunkBytes = 0;  // an `ID3 ` chunk before COMM
};
QByteArray makeAiff(const Pcm &pcm, const AiffOptions &options = {});

struct Mp3Options
{
    bool vbr = false;
    int bitrateKbps = 320; // CBR
    int vbrQuality = 0;    // VBR
    bool lameTag = true;   // the Xing/Info frame with LAME's extension
    int id3v2Bytes = 0;    // an ID3v2.3 tag of exactly this size before the audio (random "picture" bytes)
    bool id3v1 = false;    // a 128-byte ID3v1 tag after the audio
    int junkBytes = 0;     // random bytes between the ID3v2 tag and the first frame
};
QByteArray makeMp3(const Pcm &pcm, const Mp3Options &options = {});

struct FlacOptions
{
    int bits = 16;        // 16 or 24
    int pictureBytes = 0; // a PICTURE metadata block of this size (random bytes)
    int id3v2Bytes = 0;   // an ID3v2 tag before "fLaC", as some tools write
};
QByteArray makeFlac(const Pcm &pcm, FlacOptions options = {});

struct M4aOptions
{
    bool alac = false;      // AAC otherwise
    bool fastStart = false; // `moov` before `mdat`
};
// Encoded through a temporary file (the MP4 muxer reopens its output for fast start).
QByteArray makeM4a(const Pcm &pcm, M4aOptions options = {});

// FLAC in an Ogg container: a format Selecta's own demuxers don't read, so it takes the FFmpeg path.
QByteArray makeOggFlac(const Pcm &pcm);

// Opus in an Ogg container (`.opus`), with FFmpeg's own encoder at `bitrateKbps`; Opus runs at 48 kHz, so `pcm` must be.
QByteArray makeOpus(const Pcm &pcm, int bitrateKbps = 160);
// Ogg Vorbis (`.ogg`), with FFmpeg's own encoder at about `bitrateKbps`.
QByteArray makeOggVorbis(const Pcm &pcm, int bitrateKbps = 192);

// Deterministic pseudo-random bytes.
QByteArray randomBytes(int count, unsigned seed);

// RMS of the difference between two signals over their common length and channels, after skipping `skip` frames.
double rmsDifference(const Pcm &a, const Pcm &b, qint64 skip = 0);
double rms(const Pcm &pcm);

} // namespace selecta::testing
