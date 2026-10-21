// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <memory>
#include <optional>

namespace selecta::app {

// The covers of the cover wall and the album page (P-010), from the files only, never the network: an album's folder
// image (cover.jpg and the like, chosen as the converter chooses one), else its first track's embedded picture; a
// loose track's embedded picture. The files are untrusted: pictures are bounded in bytes and pixels, and decoded at
// most kSide pixels a side.
//
// Reading runs on a small pool of its own, newest request first, so the tiles on screen come before those scrolled
// past; requests waiting beyond kMaxWaiting are dropped, oldest first, and asked for again if their tile shows again.
// What was read stays in memory, the most recently used kept within kCacheBytes; "no cover" is remembered too.
//
// Threading: main thread; the pool's work holds no reference to this object, and the destructor waits for it.
class CollectionCovers : public QObject
{
    Q_OBJECT

public:
    // Where a cover may be.
    struct Source
    {
        QString folder; // an album's folder: its image first; empty for a loose track
        QString file;   // the track whose embedded picture is the cover otherwise

        friend bool operator==(const Source &, const Source &) = default;
    };

    static constexpr int kSide = 360;                             // pixels: a 180-point tile on a Retina screen
    static constexpr int kWorkers = 2;                            // files read at once
    static constexpr int kMaxWaiting = 64;                        // requests kept waiting
    static constexpr qint64 kCacheBytes = qint64{96} * 1024 * 1024; // decoded covers kept
    static constexpr qint64 kMaxPictureBytes = qint64{8} * 1024 * 1024;

    explicit CollectionCovers(QObject *parent = nullptr);
    ~CollectionCovers() override;
    CollectionCovers(const CollectionCovers &) = delete;
    CollectionCovers &operator=(const CollectionCovers &) = delete;
    CollectionCovers(CollectionCovers &&) = delete;
    CollectionCovers &operator=(CollectionCovers &&) = delete;

    // The cover `key` names, once read: a null pixmap when there's none; nullopt when it hasn't been read.
    std::optional<QPixmap> cover(const QString &key) const;
    // Reads the cover `key` names from `source`, unless it is read, being read or waiting already: ready() follows.
    void request(const QString &key, const Source &source);
    // Forgets every cover (the files changed); requests in flight are dropped when they come back.
    void clear();
    // How many requests wait or are being read. For tests.
    int pending() const { return static_cast<int>(m_waiting.size()) + m_running; }

    // Blocking: reads the cover of `source`, at most `side` pixels a side; null when there's none or it can't be read.
    // Any thread.
    static QImage read(const Source &source, int side = kSide);

Q_SIGNALS:
    // The cover `key` names was read (cover() has it, or knows there is none).
    void ready(const QString &key);

private:
    struct Request
    {
        QString key;
        Source source;
    };

    void startNext();
    void finished(const QString &key, quint64 generation, const QImage &image);

    QThreadPool m_pool;
    QList<Request> m_waiting; // oldest first; the newest is read next
    QSet<QString> m_asked;    // waiting or being read
    int m_running = 0;
    quint64 m_generation = 0; // clear() moves it on: answers from before are dropped
    QCache<QString, QPixmap> m_cache;
};

} // namespace selecta::app
