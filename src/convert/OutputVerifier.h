// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Decoder.h"
#include "audio/StreamInfo.h"
#include "convert/EncodeSettings.h"
#include "convert/MetadataModel.h"

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace selecta::convert {

// What a correct output looks like (conversion §2.5). Unset expectations aren't checked.
struct VerifyExpectations
{
    std::optional<audio::Container> container;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;        // lossless outputs
    qint64 expectedFrames = -1;   // decoded length in frames
    qint64 frameTolerance = 0;    // ± frames accepted (one MP3 frame, 1 152, for LAME outputs)
    QByteArray expectedPcmDigest; // bit-exact check (lossless at unchanged rate and depth): pcmDigest of the source
    int mp3CbrKbps = 0;           // every MPEG frame at this bitrate
    bool requireLameTag = false;  // first frame is Info/Xing with a LAME extension and a valid CRC-16
    // The text fields exactly as they must read back (TagWriter's tagsAsWritten): no field missing, none extra, every
    // value equal. Read with generous bounds (1 Mi characters per value, 20 MiB per picture).
    std::optional<MetadataModel> expectedTags;
    bool expectFrontCover = false; // exactly one picture, a front cover, that decodes
    QSize expectedCoverSize;       // when set, the cover must decode at this size
    // Decode errors, resynchronisation and truncation are problems. Off for a copy of a damaged source, whose identical
    // damage the PCM digest proves.
    bool requireCleanDecode = true;
};

struct VerifyReport
{
    audio::StreamInfo info;
    audio::DecoderStats stats;
    qint64 decodedFrames = 0;
    QByteArray pcmDigest; // of the decoded audio
    QStringList problems; // empty when the output passed

    bool passed() const { return problems.isEmpty(); }
};

// MD5 of decoded PCM: every sample as a 32-bit float, little-endian, interleaved, at the source's own rate and channel
// count. For 16- and 24-bit audio a float holds the sample exactly, so equal digests mean bit-identical audio.
// Reads the whole file: call from a worker. Failure codes: those of Decoder::open()/read(), Cancelled.
Result<QByteArray> pcmDigest(const QString &path, const audio::CancellationFlag *cancel = nullptr);

// Decodes the file completely and checks it against `expectations`: no decoder errors, stream facts, length, PCM
// digest, MP3 frame bitrates and LAME tag, tags and artwork (re-read with TagLib), and, for WAV, a RIFF INFO list with
// the title and artist (what rekordbox reads). Returns a report; problems are listed rather than stopping at the first
// one. An error (not a failed check) means the file couldn't be examined at all.
//
// Threading: reentrant; blocks, so call from a worker. Failure codes: OpenFailed, those of Decoder::open(), Cancelled.
Result<VerifyReport> verifyOutput(const QString &path, const VerifyExpectations &expectations,
                                  const audio::CancellationFlag *cancel = nullptr);

} // namespace selecta::convert
