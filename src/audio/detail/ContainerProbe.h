// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/StreamInfo.h"
#include "audio/detail/ParseStatus.h"
#include "audio/detail/SourceReader.h"

#include <QtGlobal>

namespace selecta::audio::detail {

struct ProbeResult
{
    Container container = Container::Unknown;
    qint64 leadingTagBytes = 0; // ID3v2 tags before the audio stream (MPEG audio, sometimes FLAC)
    qint64 streamStart = 0;     // first byte of the stream proper (after the tags, and junk for MPEG audio)
    // MP4: where the `moov` box ends, when it precedes `mdat`; -1 when `mdat` comes first (not progressive).
    qint64 mp4MoovEnd = -1;
};

// Identifies the container from the first bytes. Ok with Container::Unknown means "not one Selecta recognises":
// FFmpeg probes it (complete files only). NeedMoreData while a growing prefix is too short to tell (a large ID3v2 tag,
// or an MP4 whose `moov`/`mdat` order isn't visible yet).
ParseResult probeContainer(SourceReader &reader, qint64 maxHeaderBytes, ProbeResult &result);

// Looks for the first MPEG audio frame at or after `from` whose header is followed by `chain` more valid frames of the
// same stream (or by the end of a complete file). Scans at most `maxScan` bytes. Returns the offset, -1 when none
// was found, -2 when the prefix ends before it can tell (growing source).
qint64 findMpegSync(SourceReader &reader, qint64 from, qint64 maxScan, int chain);

} // namespace selecta::audio::detail
