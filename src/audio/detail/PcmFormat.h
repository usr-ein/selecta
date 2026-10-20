// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/detail/ParseStatus.h"
#include "audio/detail/SourceReader.h"

#include <QtGlobal>

// WAV and AIFF/AIFF-C headers: where the PCM samples are and how they are encoded.
namespace selecta::audio::detail {

enum class PcmEncoding { UnsignedInt, SignedInt, Float };

struct PcmLayout
{
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;  // declared word length (valid bits)
    int containerBytes = 0; // bytes per sample in the stream
    PcmEncoding encoding = PcmEncoding::SignedInt;
    bool bigEndian = false;
    qint64 dataOffset = 0;
    qint64 dataBytes = 0;    // whole frames only
    quint32 channelMask = 0; // WAVE_FORMAT_EXTENSIBLE speaker mask, 0 when not given
    bool truncated = false;  // the header announces more audio than the file holds

    int blockAlign() const { return channels * containerBytes; }
    qint64 frames() const { return blockAlign() > 0 ? dataBytes / blockAlign() : 0; }
};

// Parses a RIFF/WAVE header from the reader's prefix. Supports integer PCM (8-bit unsigned, 16/24/32-bit signed),
// IEEE float (32/64-bit) and WAVE_FORMAT_EXTENSIBLE wrapping either; chunks such as LIST, JUNK or bext before `data`
// are skipped by size. A `data` chunk before `fmt ` needs the complete file. `maxHeaderBytes` bounds where `data` may
// start. Other encodings (ADPCM, µ-law, RF64…) are Unsupported here.
ParseResult parseWav(SourceReader &reader, qint64 maxHeaderBytes, PcmLayout &layout);

// Parses a FORM/AIFF or FORM/AIFC header: integer PCM (NONE, twos, sowt, in24, in32, raw), float (fl32, fl64). COMM
// after SSND needs the complete file (audio §5.2).
ParseResult parseAiff(SourceReader &reader, qint64 maxHeaderBytes, PcmLayout &layout);

// IEEE 754 80-bit extended (AIFF sample rate), big-endian. Returns 0 for invalid or non-positive values.
double readExtended80(ByteSpan bytes);

// Converts `frames` frames of PCM in `layout`'s encoding to interleaved float in [-1, 1).
void convertPcmToFloat(const quint8 *data, qint64 frames, const PcmLayout &layout, float *out);

} // namespace selecta::audio::detail
