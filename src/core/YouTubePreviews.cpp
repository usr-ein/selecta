// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/YouTubePreviews.h"

#include "core/PreviewCache.h"
#include "meta/YtDlpRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QFuture>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

// The cache's note of the video picked for a release: what the search looks for, and the length the pick must match.
QString pickKeyOf(const meta::Release &release)
{
    const QString seconds = release.durationMs && *release.durationMs > 0
                                ? QString::number(std::llround(static_cast<double>(*release.durationMs) / 1000.0))
                                : u"-"_s;
    return u"youtube-pick\n"_s + meta::YtDlpRunner::searchText(release) + u'\n' + seconds;
}

// The cache's key for a video's audio.
QString audioKeyOf(const QString &videoId)
{
    return u"youtube\n"_s + videoId;
}

// A YouTube video id, as a note read back from the cache must be: 11 characters of [A-Za-z0-9_-].
bool isVideoId(const QString &id)
{
    return id.size() == 11 && std::ranges::all_of(id, [](QChar c) {
               return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_' ||
                      c == u'-';
           });
}

} // namespace

YouTubePreviews::YouTubePreviews(meta::YtDlpRunner &runner, PreviewCache *cache, QObject *parent)
    : QObject(parent)
    , m_runner(runner)
    , m_cache(cache)
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
    if (m_cache == nullptr || !m_cache->isUsable() || !QDir().mkpath(folder())) {
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
    m_pickKey = pickKeyOf(release);
    // Heard before: the video picked then, without searching again.
    if (const std::optional<QString> picked = m_cache->recall(m_pickKey); picked && isVideoId(*picked)) {
        QMetaObject::invokeMethod(
            this,
            [this, id, release, videoId = *picked] {
                if (id == m_current) {
                    playOrFetch(id, release, videoId, {});
                }
            },
            Qt::QueuedConnection);
        return id;
    }
    m_runner.search(meta::YtDlpRunner::searchText(release), kSearchResults)
        .then(this, [this, id, release](const meta::Result<QList<meta::YouTubeCandidate>> &result) {
            if (id != m_current) {
                return; // replaced or stopped meanwhile
            }
            if (!result.ok()) {
                fail(id, result.error().message);
                return;
            }
            const std::optional<meta::YouTubePick> pick = meta::YtDlpRunner::pick(result.value(), release);
            if (pick && pick->channel != meta::YouTubePick::Channel::Other) {
                playOrFetch(id, release, pick->candidate.id, pick->candidate.channel);
                return;
            }
            searchTopic(id, release, result.value());
        });
    return id;
}

void YouTubePreviews::searchTopic(qint64 id, const meta::Release &release, const QList<meta::YouTubeCandidate> &first)
{
    const auto pickFrom = [this, id, release](const QList<meta::YouTubeCandidate> &candidates) {
        const std::optional<meta::YouTubePick> pick = meta::YtDlpRunner::pick(candidates, release);
        if (!pick) {
            fail(id, release.durationMs ? tr("No video on YouTube matches this track's length.")
                                        : tr("Without the track's length, Selecta can't tell which video is it."));
            return;
        }
        playOrFetch(id, release, pick->candidate.id, pick->candidate.channel);
    };
    const QString text = meta::YtDlpRunner::topicSearchText(release);
    if (text.isEmpty() || !release.durationMs) {
        pickFrom(first);
        return;
    }
    // Nothing from the artist or YouTube Music yet: one more search, aimed at the artist's Topic channel.
    m_runner.search(text, kSearchResults)
        .then(this, [this, id, first, pickFrom](const meta::Result<QList<meta::YouTubeCandidate>> &result) {
            if (id != m_current) {
                return;
            }
            QList<meta::YouTubeCandidate> all = first;
            if (result.ok()) {
                for (const meta::YouTubeCandidate &candidate : result.value()) {
                    if (std::ranges::none_of(all, [&](const auto &seen) { return seen.id == candidate.id; })) {
                        all.append(candidate);
                    }
                }
            }
            pickFrom(all); // a failed second search leaves the first's pick
        });
}

void YouTubePreviews::stop()
{
    m_current = 0;
    if (m_fetch) {
        m_fetch->cancel(); // at once: yt-dlp is killed and its partial file deleted
        m_fetch->deleteLater();
        m_fetch.clear();
    }
    if (m_growing && !m_growing->snapshot().isComplete()) {
        m_growing->setFailed();
    }
    m_growing.reset();
    m_path.clear();
    hold({});
    m_videoId.clear();
    m_pickKey.clear();
}

bool YouTubePreviews::discard(qint64 id)
{
    if (id == 0 || id != m_current) {
        return false;
    }
    const QString videoId = m_videoId;
    const QString pickKey = m_pickKey;
    stop();
    if (m_cache != nullptr) {
        if (!videoId.isEmpty()) {
            m_cache->remove(audioKeyOf(videoId));
        }
        if (!pickKey.isEmpty()) {
            m_cache->forget(pickKey); // a wrong video isn't picked again without a new search
        }
    }
    return true;
}

void YouTubePreviews::playOrFetch(qint64 id, const meta::Release &release, const QString &videoId,
                                  const QString &channel)
{
    m_videoId = videoId;
    if (const std::optional<PreviewCache::File> cached = m_cache->find(audioKeyOf(videoId))) {
        if (audio::Result<std::shared_ptr<audio::FileSource>> file = audio::FileSource::open(cached->path); file) {
            hold(cached->path);
            m_cache->remember(m_pickKey, videoId);
            playable(id, release, file.takeValue(), videoId, channel.isEmpty() ? cached->note : channel);
            report(id, Preview::State::Complete, tr("From YouTube, heard before"));
            return;
        }
        m_cache->remove(audioKeyOf(videoId)); // unreadable: fetched again
    }
    fetch(id, release, videoId, channel);
}

void YouTubePreviews::fetch(qint64 id, const meta::Release &release, const QString &videoId, const QString &channel)
{
    report(id, Preview::State::Queued, tr("Fetching it from YouTube…"));
    // yt-dlp names the file "<id>.<ext>": held from now on, so emptying the cache meanwhile leaves it alone.
    hold(folder() + u'/' + videoId + u'.');
    meta::YtDlpFetch *job = m_runner.fetch(videoId, folder(), this);
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
                    m_cache->forget(m_pickKey); // the video picked may be gone: search again next time
                    fail(id, result.error().message);
                    return;
                }
                // Kept: the same release plays from the cache next time, without yt-dlp (PREV-8).
                m_cache->commit(audioKeyOf(videoId), result.value().path, channel);
                m_cache->remember(m_pickKey, videoId);
                if (m_growing && m_growing->totalSize() == result.value().bytes) {
                    m_growing->setAvailable(result.value().bytes);
                    m_growing->setComplete();
                } else {
                    // The size wasn't known while it downloaded (or changed): play the finished file.
                    audio::Result<std::shared_ptr<audio::FileSource>> file =
                        audio::FileSource::open(result.value().path);
                    if (!file) {
                        m_cache->remove(audioKeyOf(videoId));
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

void YouTubePreviews::hold(const QString &pathOrPrefix)
{
    if (m_held == pathOrPrefix) {
        return;
    }
    if (m_cache != nullptr && !m_held.isEmpty()) {
        m_cache->unpin(m_held);
    }
    m_held = pathOrPrefix;
    if (m_cache != nullptr && !m_held.isEmpty()) {
        m_cache->pin(m_held);
    }
}

QString YouTubePreviews::folder() const
{
    return m_cache != nullptr && m_cache->isUsable() ? m_cache->directory() + u"/YouTube"_s : QString();
}

} // namespace selecta::core
