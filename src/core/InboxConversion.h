// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "convert/EncodeSettings.h"
#include "convert/MetadataModel.h"
#include "convert/QualityAdvisor.h"
#include "convert/Transcoder.h"

#include <QByteArray>
#include <QString>

#include <optional>

// What singles and albums share on their way into the Inbox (PRD D29, DL-3): the Inbox format, the cover, and the
// verified conversion. Pure or file-bound functions; call them from a worker.
namespace selecta::core {

// "FLAC 16/44.1", "MP3 320", "MP3 VBR 245".
QString sourceQualityText(const convert::SourceQuality &quality);

// What the Inbox gets from a file of this quality: MP3 320 CBR (D29), or the audio kept as it is when converting it
// can't improve it (lossy audio below the format).
struct InboxFormat
{
    convert::EncodeSettings settings;
    bool kept = false;
    QString extension;          // of the file in the Inbox
    QString format;             // as the Inbox shows it: "MP3", "AAC", "WAV"…
    std::optional<int> bitrate; // lossy formats
};
InboxFormat inboxFormatFor(const convert::SourceQuality &quality, const QString &sourceSuffix);

// Makes `cover` (or, when empty, the picture the tags already have) the one front cover, processed for CDJs: JPEG, at
// most 800×800. A cover that can't be used leaves the tags without one; `why` says so.
void attachCover(convert::MetadataModel &tags, const QByteArray &cover, QString *why = nullptr);

// Converts (or copies, when kept) `source` into `output` with exactly `tags`, verified: it decodes whole and its tags
// read back. `encodedBy` goes in TENC.
convert::Result<convert::TranscodeReport> writeInboxFile(const QString &source, const QString &output,
                                                         const InboxFormat &format, const convert::MetadataModel &tags,
                                                         const QString &encodedBy,
                                                         const audio::CancellationFlag *cancel);

} // namespace selecta::core
