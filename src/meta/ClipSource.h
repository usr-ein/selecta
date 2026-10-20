// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Release.h"
#include "meta/Result.h"
#include "meta/TextMatcher.h"

#include <QDateTime>
#include <QFuture>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

namespace selecta::meta {

class DeezerClient;

// An official 30-second clip to stream (META-5). Only the URL: the audio is never stored (Deezer's terms forbid
// local storage of audio, R4), so the player streams it from memory and shows the attribution and link-back.
struct Clip
{
    QUrl url;            // signed MP3 URL; use it before expiresAt
    QDateTime expiresAt; // UTC
    QUrl pageUrl;        // the track on Deezer (link-back)
    Source source = Source::Deezer;
    int durationSec = 30;
    QString deezerTrackId;
};

// Finds the official clip for a release (META-5, §9.1): Deezer by ISRC, else a Deezer search on the main artist and
// base title, keeping the best hit whose version agrees and whose length is within ±3 s of the release (when the
// release has a length). Always asks Deezer for a fresh signed URL (they expire after ~15 minutes).
//
// Threading: lives on the meta thread; futures finish there. Ownership: the Deezer client must outlive this object.
// Failures: NotFound when Deezer has no matching track or the track has no preview; otherwise the client's errors.
class ClipSource : public QObject
{
    Q_OBJECT

public:
    ClipSource(DeezerClient &deezer, std::shared_ptr<const TextMatcher> matcher, QObject *parent = nullptr);
    ~ClipSource() override;
    ClipSource(const ClipSource &) = delete;
    ClipSource &operator=(const ClipSource &) = delete;
    ClipSource(ClipSource &&) = delete;
    ClipSource &operator=(ClipSource &&) = delete;

    QFuture<Result<Clip>> clipFor(const Release &release);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
