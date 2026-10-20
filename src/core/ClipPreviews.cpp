// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ClipPreviews.h"

#include "meta/ClipSource.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

using namespace Qt::StringLiterals;

namespace selecta::core {

ClipPreviews::ClipPreviews(meta::ClipSource &clips, QNetworkAccessManager &network, QObject *parent)
    : QObject(parent)
    , m_clips(clips)
    , m_network(network)
{}

ClipPreviews::~ClipPreviews()
{
    stop();
}

qint64 ClipPreviews::start(const QString &key, const meta::Release &release, Unavailable unavailable)
{
    stop();
    const qint64 id = m_nextId++;
    m_current = id;
    m_key = key;
    m_unavailable = std::move(unavailable);
    report(id, Preview::State::Queued, tr("Looking for its 30-second clip…"));
    m_clips.clipFor(release).then(this, [this, id, release](const meta::Result<meta::Clip> &clip) {
        if (id != m_current) {
            return;
        }
        if (!clip.ok()) {
            fail(id, clip.error().message);
            return;
        }
        fetch(id, release, clip.value().url, clip.value().deezerTrackId);
    });
    return id;
}

void ClipPreviews::stop()
{
    m_current = 0;
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
}

void ClipPreviews::fetch(qint64 id, const meta::Release &release, const QUrl &url, const QString &trackId)
{
    if (url.scheme() != u"https"_s) {
        fail(id, tr("Deezer's clip address isn't a secure one."));
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(20'000);
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    reply->setReadBufferSize(kMaxClipBytes + 1);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, id, reply](qint64 received, qint64 /*total*/) {
        if (received > kMaxClipBytes && id == m_current) {
            reply->abort(); // finished() reports it
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, release, trackId] {
        reply->deleteLater();
        if (id != m_current) {
            return;
        }
        m_reply.clear();
        const QByteArray data = reply->read(kMaxClipBytes + 1);
        if (reply->error() != QNetworkReply::NoError || data.isEmpty() || data.size() > kMaxClipBytes) {
            fail(id, tr("The clip couldn't be fetched: %1").arg(reply->errorString()));
            return;
        }
        report(id, Preview::State::Complete, tr("30-second clip from Deezer"));
        Q_EMIT started(m_key, id, std::make_shared<audio::MemorySource>(data), u"clip|deezer|"_s + trackId,
                       release.title, release.artist(), tr("30-second clip · Deezer"));
    });
}

void ClipPreviews::fail(qint64 id, const QString &message)
{
    report(id, Preview::State::Failed, message);
    if (id == m_current && m_unavailable) {
        std::exchange(m_unavailable, {})(message);
    }
}

void ClipPreviews::report(qint64 id, Preview::State state, const QString &message)
{
    Preview preview;
    preview.id = id;
    preview.peer = u"Deezer"_s;
    preview.state = state;
    preview.message = message;
    Q_EMIT changed(preview);
}

} // namespace selecta::core
