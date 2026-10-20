// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/YouTubePreviews.h"

#include "meta/YtDlpRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QFuture>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {

YouTubePreviews::YouTubePreviews(meta::YtDlpRunner &runner, QString cacheDir, QObject *parent)
    : QObject(parent)
    , m_runner(runner)
    , m_cacheDir(std::move(cacheDir))
{}

YouTubePreviews::~YouTubePreviews()
{
    stop();
}

qint64 YouTubePreviews::start(const QString &key, const meta::Release &release, Unavailable unavailable)
{
    stop();
    const qint64 id = m_nextId++;
    m_current = id;
    m_key = key;
    m_unavailable = std::move(unavailable);
    m_announced = false;
    report(id, Preview::State::Queued, tr("Looking for it on YouTube…"));
    if (m_cacheDir.isEmpty() || !QDir().mkpath(m_cacheDir)) {
        // On the next turn, like every other outcome: the caller gets the id first.
        QMetaObject::invokeMethod(
            this,
            [this, id] {
                if (id == m_current) {
                    fail(id, tr("Previews have nowhere to go."));
                }
            },
            Qt::QueuedConnection);
        return id;
    }
    m_runner.search(meta::YtDlpRunner::searchText(release), 10)
        .then(this, [this, id, release](const meta::Result<QList<meta::YouTubeCandidate>> &result) {
            if (id != m_current) {
                return; // replaced or stopped meanwhile
            }
            if (!result.ok()) {
                fail(id, result.error().message);
                return;
            }
            const std::optional<meta::YouTubePick> pick = meta::YtDlpRunner::pick(result.value(), release);
            if (!pick) {
                fail(id, release.durationMs ? tr("No video on YouTube matches this track's length.")
                                            : tr("Without the track's length, Selecta can't tell which video is it."));
                return;
            }
            fetch(id, release, pick->candidate.id, pick->candidate.channel);
        });
    return id;
}

void YouTubePreviews::stop()
{
    m_current = 0;
    if (m_fetch) {
        m_fetch->deleteLater(); // cancels: yt-dlp is killed and its partial file deleted
        m_fetch.clear();
    }
    if (m_growing && !m_growing->snapshot().isComplete()) {
        m_growing->setFailed();
    }
    m_growing.reset();
    m_path.clear();
}

void YouTubePreviews::fetch(qint64 id, const meta::Release &release, const QString &videoId, const QString &channel)
{
    report(id, Preview::State::Queued, tr("Fetching it from YouTube…"));
    meta::YtDlpFetch *job = m_runner.fetch(videoId, m_cacheDir, this);
    m_fetch = job;
    connect(job, &meta::YtDlpFetch::started, this, [this, id](const QString &path) {
        if (id == m_current) {
            m_path = path;
        }
    });
    connect(
        job, &meta::YtDlpFetch::progress, this, [this, id, release, videoId, channel](qint64 downloaded, qint64 total) {
            if (id != m_current || m_path.isEmpty() || total <= 0) {
                return;
            }
            // Only what has reached the disk is readable: yt-dlp may count bytes it still buffers.
            const qint64 onDisk = std::min(downloaded, QFileInfo(m_path).size());
            if (!m_growing) {
                audio::Result<std::shared_ptr<audio::GrowingFile>> growing = audio::GrowingFile::open(m_path, total);
                if (!growing) {
                    return; // played from the finished file instead
                }
                m_growing = growing.takeValue();
                m_growing->setAvailable(onDisk);
                playable(id, release, m_growing, videoId, channel);
            } else {
                m_growing->setAvailable(onDisk);
            }
        });
    connect(job, &meta::YtDlpFetch::finished, this,
            [this, id, release, videoId, channel](const meta::Result<meta::FetchedAudio> &result) {
                if (id != m_current) {
                    return;
                }
                m_fetch.clear();
                if (!result.ok()) {
                    if (m_growing) {
                        m_growing->setFailed();
                    }
                    fail(id, result.error().message);
                    return;
                }
                if (m_growing && m_growing->totalSize() == result.value().bytes) {
                    m_growing->setAvailable(result.value().bytes);
                    m_growing->setComplete();
                } else {
                    // The size wasn't known while it downloaded (or changed): play the finished file.
                    audio::Result<std::shared_ptr<audio::FileSource>> file =
                        audio::FileSource::open(result.value().path);
                    if (!file) {
                        fail(id, file.error().message);
                        return;
                    }
                    m_announced = false;
                    playable(id, release, file.takeValue(), videoId, channel);
                }
                report(id, Preview::State::Complete, tr("From YouTube"));
            });
}

void YouTubePreviews::playable(qint64 id, const meta::Release &release,
                               const std::shared_ptr<audio::ByteSource> &source, const QString &videoId,
                               const QString &channel)
{
    if (m_announced) {
        return;
    }
    m_announced = true;
    Q_EMIT started(m_key, id, source, u"youtube|"_s + videoId, release.title, release.artist(),
                   channel.isEmpty() ? tr("YouTube (lossy preview)") : tr("YouTube · %1 (lossy preview)").arg(channel));
}

void YouTubePreviews::fail(qint64 id, const QString &message)
{
    report(id, Preview::State::Failed, message);
    if (id == m_current && !m_announced && m_unavailable) {
        std::exchange(m_unavailable, {})(message);
    }
}

void YouTubePreviews::report(qint64 id, Preview::State state, const QString &message)
{
    Preview preview;
    preview.id = id;
    preview.peer = u"YouTube"_s;
    preview.state = state;
    preview.message = message;
    Q_EMIT changed(preview);
}

} // namespace selecta::core
