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

// Previews from YouTube through the user's own yt-dlp (PRD D26, PREV-11, ADR-011): the release's track is searched,
// the video within ±3 s of its length is picked, and its audio is fetched into the preview cache and played while it
// arrives. The audio is lossy and is never filed, shared, converted or tagged. One at a time.
//
// Threading: engine thread. Ownership: the runner (MetaServices) outlives it.
class YouTubePreviews : public QObject
{
    Q_OBJECT

public:
    YouTubePreviews(meta::YtDlpRunner &runner, QString cacheDir, QObject *parent = nullptr);
    ~YouTubePreviews() override;
    YouTubePreviews(const YouTubePreviews &) = delete;
    YouTubePreviews &operator=(const YouTubePreviews &) = delete;
    YouTubePreviews(YouTubePreviews &&) = delete;
    YouTubePreviews &operator=(YouTubePreviews &&) = delete;

    // Runs when no audio can come, before any started(): yt-dlp missing, nothing within ±3 s, a failed fetch.
    using Unavailable = std::function<void(const QString &why)>;

    // Starts looking for the release's track, replacing the preview before. `key` names what the preview is for
    // (core::previewKeyForLine() and the like). Returns the preview's id.
    qint64 start(const QString &key, const meta::Release &release, Unavailable unavailable = {});
    void stop();

Q_SIGNALS:
    void started(const QString &key, qint64 previewId, const std::shared_ptr<selecta::audio::ByteSource> &source,
                 const QString &identity, const QString &title, const QString &artist, const QString &label);
    // Peer "YouTube": what is happening (Queued while searching and fetching), then Complete or Failed.
    void changed(const selecta::core::Preview &preview);

private:
    void fetch(qint64 id, const meta::Release &release, const QString &videoId, const QString &channel);
    void report(qint64 id, Preview::State state, const QString &message);
    // A failure: reported, and handed to the caller's fallback when nothing played yet.
    void fail(qint64 id, const QString &message);
    void playable(qint64 id, const meta::Release &release, const std::shared_ptr<audio::ByteSource> &source,
                  const QString &videoId, const QString &channel);

    meta::YtDlpRunner &m_runner;
    QString m_cacheDir;
    qint64 m_current = 0; // the preview id, 0 when none
    QString m_key;
    Unavailable m_unavailable;
    qint64 m_nextId = static_cast<qint64>(1ULL << 40U); // apart from the transfer service's preview ids
    QPointer<meta::YtDlpFetch> m_fetch;
    QString m_path;                                // the file yt-dlp writes
    std::shared_ptr<audio::GrowingFile> m_growing; // once its size is known
    bool m_announced = false;                      // started() was emitted for m_current
};

} // namespace selecta::core
