// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "core/TransferService.h"
#include "meta/Release.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

namespace selecta::meta {
class YtDlpFetch;
class YtDlpRunner;
} // namespace selecta::meta

namespace selecta::core {

class PreviewCache;

// Previews from YouTube through the user's own yt-dlp (PRD D26, PREV-11, ADR-011): the release's track is searched,
// the video within ±3 s of its length is picked, and its audio is fetched into the preview cache and played while it
// arrives. The audio is lossy and is never filed, shared, converted or tagged. One at a time.
//
// The cache keeps the audio (in its YouTube folder) and which video was picked for the release, so a track heard
// before plays again at once, with no yt-dlp run, after a restart too (PREV-8).
//
// Threading: engine thread. Ownership: the runner (MetaServices) and the preview cache outlive it.
class YouTubePreviews : public QObject
{
    Q_OBJECT

public:
    // A null cache: previews have nowhere to go, and every start() fails.
    YouTubePreviews(meta::YtDlpRunner &runner, PreviewCache *cache, QObject *parent = nullptr);
    ~YouTubePreviews() override;
    YouTubePreviews(const YouTubePreviews &) = delete;
    YouTubePreviews &operator=(const YouTubePreviews &) = delete;
    YouTubePreviews(YouTubePreviews &&) = delete;
    YouTubePreviews &operator=(YouTubePreviews &&) = delete;

    // Runs when no audio can come, before any started(): yt-dlp missing, nothing within ±3 s, a failed fetch.
    using Unavailable = std::function<void(const QString &why)>;

    // Starts looking for the release's track, replacing the preview before. `key` names what the preview is for
    // (core::previewKeyForLine() and the like). Returns the preview's id; everything else is reported later.
    qint64 start(const QString &key, const meta::Release &release, Unavailable unavailable = {});
    // Stops the preview: a fetch in flight is cancelled at once, its partial file deleted.
    void stop();
    // Discard (PREV-5): when `id` is the current preview, it stops and its audio leaves the cache, with the video
    // picked for its release. False for any other id.
    bool discard(qint64 id);

Q_SIGNALS:
    void started(const QString &key, qint64 previewId, const std::shared_ptr<selecta::audio::ByteSource> &source,
                 const QString &identity, const QString &title, const QString &artist, const QString &label);
    // Peer "YouTube": what is happening (Queued while searching and fetching), then Complete or Failed.
    void changed(const selecta::core::Preview &preview);

private:
    // The video's audio: from the cache when it is there, else fetched.
    void playOrFetch(qint64 id, const meta::Release &release, const QString &videoId, const QString &channel);
    void fetch(qint64 id, const meta::Release &release, const QString &videoId, const QString &channel);
    void report(qint64 id, Preview::State state, const QString &message);
    // A failure: reported, and handed to the caller's fallback when nothing played yet.
    void fail(qint64 id, const QString &message);
    void playable(qint64 id, const meta::Release &release, const std::shared_ptr<audio::ByteSource> &source,
                  const QString &videoId, const QString &channel);
    // What the current preview holds in the cache (a file, or the prefix of the files yt-dlp writes); empty: nothing.
    void hold(const QString &pathOrPrefix);
    QString folder() const;

    meta::YtDlpRunner &m_runner;
    PreviewCache *m_cache;
    qint64 m_current = 0; // the preview id, 0 when none
    QString m_key;
    Unavailable m_unavailable;
    qint64 m_nextId = static_cast<qint64>(1ULL << 40U); // apart from the transfer service's preview ids
    QPointer<meta::YtDlpFetch> m_fetch;
    QString m_path;                                // the file yt-dlp writes
    std::shared_ptr<audio::GrowingFile> m_growing; // once its size is known
    bool m_announced = false;                      // started() was emitted for m_current
    QString m_pickKey;                             // the cache's note of the video picked for the current release
    QString m_videoId;                             // the current preview's video, once picked
    QString m_held;                                // what hold() pinned in the cache
};

} // namespace selecta::core
