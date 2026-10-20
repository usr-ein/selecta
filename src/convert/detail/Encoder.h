// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "convert/EncodeSettings.h"
#include "convert/detail/OutputFile.h"

#include <QtGlobal>

#include <memory>

namespace selecta::convert::detail {

// The PCM an encoder receives.
struct EncoderFormat
{
    int sampleRate = 0;
    int channels = 0;    // 1 or 2 (more for the lossless encoders)
    int bitDepth = 0;    // lossless outputs: 16 or 24; 0 for MP3
    bool dither = false; // TPDF dither when quantising to 16-bit
};

// One output stream being encoded into a Sink. Created by the factories below, which write the stream's header.
//
// Threading: one thread at a time.
class Encoder
{
public:
    Encoder() = default;
    virtual ~Encoder() = default;
    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;
    Encoder(Encoder &&) = delete;
    Encoder &operator=(Encoder &&) = delete;

    // Encodes `frames` interleaved float frames in [-1, 1]. Failure codes: EncoderError, DiskFull, WriteFailed,
    // TooLarge (WAV and AIFF stop at 4 GiB).
    virtual Result<void> write(const float *samples, qint64 frames) = 0;
    // Drains the codec and completes the stream (sizes, seek tables and the LAME tag patched in). Call once, after the
    // last write(). Same failure codes.
    virtual Result<void> finish() = 0;
    // Samples beyond full scale that had to be clipped while quantising (0 for MP3, which takes float).
    virtual qint64 clippedSamples() const { return 0; }
};

// libmp3lame driven directly (conversion §2.1): CBR or VBR, joint stereo (mono stays mono), float input, and the
// genuine LAME Info/Xing tag written over the first frame by finish(). lame_init()/lame_init_params() are serialised
// process-wide. Failure codes: InvalidArgument (a rate other than 32, 44.1 or 48 kHz, more than two channels),
// EncoderError.
Result<std::unique_ptr<Encoder>> createMp3Encoder(Mp3Settings settings, EncoderFormat format, Sink &sink);

// FFmpeg's FLAC, ALAC (in the `ipod` M4A muxer) and AIFF (big-endian PCM, so `FORM/AIFF` rather than AIFC) through a
// write AVIOContext over the sink. 16-bit audio goes in as 16-bit and 24-bit as 32-bit with bits_per_raw_sample = 24,
// so 16-bit never becomes padded 24-bit (conversion §2.4). Failure codes: InvalidArgument (another format, a depth
// other than 16 or 24), EncoderError, DiskFull, WriteFailed.
Result<std::unique_ptr<Encoder>> createFfmpegEncoder(OutputFormat format, EncoderFormat pcm, Sink &sink);

// Selecta's own RIFF/WAVE writer: WAVE_FORMAT_PCM (never WAVE_FORMAT_EXTENSIBLE, which older CDJs reject; conversion
// §2.3), 16- or 24-bit little-endian, `fmt ` then `data`. Failure codes: InvalidArgument, DiskFull, WriteFailed,
// TooLarge.
Result<std::unique_ptr<Encoder>> createWavEncoder(EncoderFormat pcm, Sink &sink);

// The encoder for `format` (not KeepAudio).
Result<std::unique_ptr<Encoder>> createEncoder(const EncodeSettings &settings, EncoderFormat pcm, Sink &sink);

} // namespace selecta::convert::detail
