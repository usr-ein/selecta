// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/Decoder.h"
#include "audio/Error.h"
#include "audio/StreamInfo.h"

#include <QtGlobal>

#include <memory>
#include <vector>

// A decoding backend turns a source into chunks of interleaved float audio at the source rate and channel count,
// positioned on the stream's raw timeline (sample 0 = the first sample the codec produces, before any gapless
// trimming). The Decoder trims, maps channels, resamples and serves reads on top.
namespace selecta::audio::detail {

struct DecodedChunk
{
    std::vector<float> samples; // interleaved
    int channels = 0;
    qint64 firstSample = 0; // raw timeline position of the first frame

    qint64 frames() const { return channels > 0 ? static_cast<qint64>(samples.size()) / channels : 0; }
    void clear()
    {
        samples.clear();
        firstSample = 0;
    }
};

// Raw samples to drop: [begin, end). Encoder priming and padding (gapless MP3).
struct DiscardWindow
{
    qint64 begin = 0;
    qint64 end = 0;
};

class Backend
{
public:
    enum class Status { Ok, NeedMoreData, EndOfStream, Error };

    Backend() = default;
    virtual ~Backend() = default;
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;
    Backend(Backend &&) = delete;
    Backend &operator=(Backend &&) = delete;

    // Parses the stream header and opens the codec. NeedMoreData when a growing prefix is too short. On Error, the
    // error's code may be NeedsCompleteFile.
    virtual Status open(Error &error) = 0;
    virtual const StreamInfo &info() const = 0;
    // Raw length in samples (before trimming), -1 when unknown.
    virtual qint64 rawLength() const = 0;
    virtual std::vector<DiscardWindow> discardWindows() const = 0;

    // Decodes the next chunk (replacing `chunk`'s contents). Error codes include SourceFailed when the source stopped
    // before the data needed.
    virtual Status decode(DecodedChunk &chunk, Error &error) = 0;
    // Positions the backend so that decoding resumes at or before `rawSample` (preroll included). `resumeAt` is the
    // raw position of the next chunk when the backend knows it, else -1 (the chunks carry their positions).
    virtual Status seek(qint64 rawSample, qint64 &resumeAt, Error &error) = 0;
    // Raw samples decodable with the bytes available now.
    virtual qint64 decodableRawSamples() = 0;
    virtual DecoderStats stats() const = 0;

    // A new backend on the same complete source, sharing immutable state where possible, opened and at the start.
    virtual Result<std::unique_ptr<Backend>> clone() const = 0;
};

} // namespace selecta::audio::detail
