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

class QNetworkAccessManager;
class QNetworkReply;

namespace selecta::meta {
class ClipSource;
}

namespace selecta::core {

// Official 30-second clips (PRD META-5, PREV-12): Deezer's preview of the release's track, found by ISRC or by name
// and length, fetched into memory and played from there. Never stored on disk (Deezer's terms forbid it), never
// filed. One at a time.
//
// Threading: engine thread. Ownership: the clip source and the network manager (MetaServices) outlive it.
class ClipPreviews : public QObject
{
    Q_OBJECT

public:
    using Unavailable = std::function<void(const QString &why)>;
    static constexpr qint64 kMaxClipBytes = qint64{4} * 1024 * 1024; // 30 s of MP3 is about half a megabyte

    ClipPreviews(meta::ClipSource &clips, QNetworkAccessManager &network, QObject *parent = nullptr);
    ~ClipPreviews() override;
    ClipPreviews(const ClipPreviews &) = delete;
    ClipPreviews &operator=(const ClipPreviews &) = delete;
    ClipPreviews(ClipPreviews &&) = delete;
    ClipPreviews &operator=(ClipPreviews &&) = delete;

    // Starts finding and fetching the release's clip, replacing the preview before. `unavailable` runs when no clip
    // can play (none on Deezer, a failed fetch). Returns the preview's id.
    qint64 start(const QString &key, const meta::Release &release, Unavailable unavailable = {});
    void stop();

Q_SIGNALS:
    void started(const QString &key, qint64 previewId, const std::shared_ptr<selecta::audio::ByteSource> &source,
                 const QString &identity, const QString &title, const QString &artist, const QString &label);
    // Peer "Deezer": what is happening, then Complete or Failed.
    void changed(const selecta::core::Preview &preview);

private:
    void fetch(qint64 id, const meta::Release &release, const QUrl &url, const QString &trackId);
    void report(qint64 id, Preview::State state, const QString &message);
    void fail(qint64 id, const QString &message);

    meta::ClipSource &m_clips;
    QNetworkAccessManager &m_network;
    qint64 m_current = 0;
    QString m_key;
    Unavailable m_unavailable;
    qint64 m_nextId = static_cast<qint64>(1ULL << 41U); // apart from the other previews' ids
    QPointer<QNetworkReply> m_reply;
};

} // namespace selecta::core
