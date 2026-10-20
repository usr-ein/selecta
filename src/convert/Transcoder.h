// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "audio/StreamInfo.h"
#include "convert/EncodeSettings.h"
#include "convert/MetadataModel.h"
#include "convert/OutputVerifier.h"
#include "convert/TagPolicy.h"
#include "convert/TagWriter.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace selecta::convert {

struct TranscodeRequest
{
    QString sourcePath;
    // Where the verified output appears. It must not exist, and its directory must. The work happens in a temporary
    // file next to it ("<name>.<random>.selecta-part"), renamed to outputPath without overwriting only once the
    // output has been verified; on any failure or cancellation the temporary file is deleted. Nothing else on disk is
    // touched: publishing into the Inbox and trashing the source belong to core's FileOps (ADR-009).
    QString outputPath;
    EncodeSettings settings;
    // The tags to write, typically the matched release merged over the file's own tags. The policy is applied first
    // (applyTagPolicy): with policy.transcoding set automatically for re-encoded audio. The front cover, if any, must
    // already be processed (ArtworkProcessor); it is embedded as is.
    MetadataModel tags;
    TagPolicyOptions policy;
    TagWriteOptions tagOptions;
};

struct TranscodeReport
{
    QString outputPath;
    audio::StreamInfo source;
    VerifyReport verification;
    int outputSampleRate = 0;
    int outputBitDepth = 0; // 0 for MP3
    bool resampled = false; // libsoxr was used (I-RESAMPLE)
    bool dithered = false;  // TPDF dither to 16-bit (I-DITHER)
    qint64 outputBytes = 0;
};

// Progress in [0, 1], from the worker thread. Must be thread-safe and quick.
using TranscodeProgress = std::function<void(double fraction)>;

// Converts one file (conversion §2, §9.1; ADR-003): decode with FFmpeg → libsoxr resampling only when needed → TPDF
// dither only when reducing to 16-bit → encode (libmp3lame directly with a genuine LAME Info tag; FFmpeg's FLAC
// encoder; FFmpeg's AIFF muxer with big-endian PCM; Selecta's own WAVE_FORMAT_PCM writer) → tags with TagLib →
// OutputVerifier → rename into place. KeepAudio copies the audio bytes and rewrites only the tags, then checks that the
// decoded audio is bit-identical to the source.
//
// Threading: reentrant and blocking: run it on a worker (TranscodeJob does). Many conversions may run in parallel;
// LAME's non-thread-safe initialisation is serialised internally. Cancellation is checked between buffers.
//
// Failure codes: OutputExists, OpenFailed, InvalidArgument (bad settings), those of Decoder::open()/read(),
// EncoderError, WriteFailed, DiskFull, TagError, VerificationFailed (with the problems in the message), Cancelled.
Result<TranscodeReport> transcode(const TranscodeRequest &request, const audio::CancellationFlag *cancel = nullptr,
                                  const TranscodeProgress &progress = {});

// Asynchronous transcode for the UI and the arrival pipeline. Lives in the thread that created it (which must run an
// event loop); signals arrive there. Destroying the job cancels it: the worker stops at the next buffer and deletes
// its temporary file, and nothing touches the destroyed job.
class TranscodeJob : public QObject
{
    Q_OBJECT

public:
    explicit TranscodeJob(TranscodeRequest request, QObject *parent = nullptr);
    ~TranscodeJob() override;
    TranscodeJob(const TranscodeJob &) = delete;
    TranscodeJob &operator=(const TranscodeJob &) = delete;
    TranscodeJob(TranscodeJob &&) = delete;
    TranscodeJob &operator=(TranscodeJob &&) = delete;

    const TranscodeRequest &request() const { return m_request; }

    // Runs on QThreadPool::globalInstance(). Starts once; later calls do nothing.
    void start();
    // No signal is emitted after cancel() returns; the worker cleans up in the background.
    void cancel();

Q_SIGNALS:
    void progress(double fraction); // coalesced to about 10 updates per second
    void finished(const selecta::convert::TranscodeReport &report);
    void failed(const selecta::audio::Error &error);

private Q_SLOTS:
    void drainMailbox();

private:
    struct Shared;
    struct Message;
    static void post(const std::shared_ptr<Shared> &shared, Message message);

    TranscodeRequest m_request;
    std::shared_ptr<Shared> m_shared;
    bool m_started = false;
    bool m_running = false;
};

} // namespace selecta::convert

Q_DECLARE_METATYPE(selecta::convert::TranscodeReport)
