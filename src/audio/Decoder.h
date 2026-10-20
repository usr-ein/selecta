// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/Error.h"
#include "audio/StreamInfo.h"

#include <QtGlobal>

#include <memory>

namespace selecta::audio {

// Resampling quality when DecoderOptions::sampleRate differs from the source rate (libsoxr recipes).
enum class ResamplerQuality {
    High,     // SOXR_HQ, 20-bit precision: playback
    VeryHigh, // SOXR_VHQ, 28-bit precision: transcoding (conversion §2.2)
};

struct DecoderOptions
{
    int sampleRate = 0; // output rate in Hz, 8 000…384 000; 0 keeps the source rate (no resampling)
    int channels = 2;   // 1: mono mix (L+R)/2; 2: stereo (mono duplicated, more channels downmixed); 0: source
                        // channels, unmixed
    ResamplerQuality resampler = ResamplerQuality::High;

    // Safety bounds for untrusted files. Exceeding one fails open() with TooLarge.
    qint64 maxHeaderBytes = qint64{64} * 1024 * 1024; // tags and metadata before the audio (ID3v2 with artwork, FLAC
                                                      // PICTURE blocks, an MP4 `moov`)
    qint64 maxDurationSeconds = qint64{12} * 3600;    // a longer declared duration is treated as corrupt
};

enum class DecodeStatus {
    Ok,           // progress was made; call again
    NeedMoreData, // a growing source hasn't delivered the bytes needed yet; call again after it grows
    EndOfStream,  // everything has been decoded
    Error,        // see Decoder::error(); the decoder is unusable until reopened
};

struct ReadResult
{
    qint64 frames = 0; // frames written to the destination
    DecodeStatus status = DecodeStatus::Ok;
};

// Counters that tell whether a file decoded cleanly (OutputVerifier, quality checks).
struct DecoderStats
{
    qint64 decodeErrors = 0; // packets the codec rejected (their audio is replaced by silence of the same length)
    qint64 skippedBytes = 0; // bytes skipped while resynchronising on corrupt data
    bool truncated = false;  // a complete source ended inside a frame, or before the length its header announced
};

// Decodes an audio file to interleaved float PCM in [-1, 1], at a requested rate and channel layout.
//
// Formats: MP3/MP2 and native FLAC, WAV and AIFF are read by Selecta's own demuxers, which index every frame as bytes
// arrive; they decode while the file downloads and seek exactly. MP4/M4A goes through FFmpeg's demuxer and decodes
// while downloading only when its `moov` index comes before the audio ("fast start"); otherwise open() returns
// NeedsCompleteFile until the source is complete. Every other format FFmpeg supports (Ogg, WMA, APE, WavPack, …) needs
// the complete file. Codecs are always FFmpeg's (references/audio-preview-and-waveform.md §3–5).
//
// Growing files: the decoder never reads at or beyond the source's available() count. FFmpeg sees a non-seekable
// input until the source completes, so no demuxer can wander to the end of the file. When the decoder needs bytes that
// haven't arrived, read() and open() return NeedMoreData at once: nothing blocks. Call again after the source grows
// (ByteSource::subscribe()).
//
// Positions are in output frames: frame 0 is the first audible sample. MP3 encoder delay and padding recorded in a LAME
// tag are removed (gapless), so a LAME-encoded MP3 decodes to exactly its source's length.
//
// Threading: not thread-safe. One thread at a time; it may move between threads if the hand-over is synchronised (a
// queued signal, a thread-pool task boundary). Several decoders may read the same ByteSource concurrently.
//
// Ownership: the decoder shares ownership of its source. Errors: open(), read() and seek() report failures through
// their status; error() then says why. After an error the decoder must be reopened.
class Decoder
{
public:
    enum class OpenStatus {
        Ready,             // decoding can start; info() is valid
        NeedMoreData,      // a growing source doesn't yet hold the whole header; call open() again after it grows
        NeedsCompleteFile, // this file can't be decoded while it downloads; call open() again once it is complete
        Failed,            // see error()
    };

    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    Decoder(Decoder &&) noexcept;
    Decoder &operator=(Decoder &&) noexcept;

    // Opens `source`, replacing any previous one. Failure codes: Unsupported (not audio, or a codec this build lacks),
    // Corrupt, TooLarge (DecoderOptions bounds), ReadFailed, SourceFailed (the download failed before the header
    // arrived), InvalidArgument (options out of range).
    [[nodiscard]] OpenStatus open(std::shared_ptr<ByteSource> source, const DecoderOptions &options = {});
    bool isOpen() const;
    void close();

    // Valid after open() returned Ready.
    const StreamInfo &info() const;
    int sampleRate() const; // output rate
    int channels() const;   // output channels
    // Length in output frames (after gapless trimming), or -1 when unknown. May be an estimate for some formats: see
    // info().exactLength.
    qint64 lengthFrames() const;

    // Decodes up to `maxFrames` frames into `destination` (room for maxFrames × channels() floats). Returns the frames
    // written and a status. A short count with Ok is normal. NeedMoreData may come with frames > 0: use them, then
    // wait. EndOfStream comes with the last frames (possibly 0). Failure codes: DecoderError, ReadFailed, SourceFailed
    // (the download failed before the data the decoder needed), Corrupt.
    ReadResult read(float *destination, qint64 maxFrames);

    // The output frame the next read() returns, or the pending seek target.
    qint64 position() const;

    // Moves to output frame `frame` (clamped to 0…lengthFrames()). Exact when info().exactSeek; otherwise the first
    // frame decoded after the seek is the nearest one FFmpeg can reach. Returns Ok when done, NeedMoreData when the
    // target's bytes haven't arrived (the seek stays pending: position() reports the target and read() returns
    // NeedMoreData until they do), Error on failure.
    DecodeStatus seek(qint64 frame);

    // How many output frames, from frame 0, can be decoded with the bytes available now: the length for a complete
    // source; for a growing one, the frames whose data has fully arrived. For the "buffered" overlay and the decision
    // to wait for data (PREV-2/3).
    qint64 decodableFrames() const;

    DecoderStats stats() const;
    const Error &error() const;

    // A second decoder on the same source and stream index, positioned at frame 0, with other output options. Only for
    // complete sources: the frame index is shared, not rebuilt, which is what makes chunked parallel analysis cheap.
    // For a growing source, open another Decoder. Fails with InvalidArgument if this decoder isn't Ready or its source
    // isn't complete.
    Result<std::unique_ptr<Decoder>> clone(const DecoderOptions &options) const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace selecta::audio
