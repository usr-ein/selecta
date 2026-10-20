// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <chrono>
#include <list>
#include <optional>

namespace selecta::meta {

// A small key → bytes cache with a lifetime per entry. The clients store raw service answers in it, keyed by
// service and request (never by anything secret), with the lifetimes of ServicePolicy.h: search answers 24 h,
// tracks, albums and MusicBrainz lookups 30 days, "not found" answers 24 h. Keys start with a service prefix
// ("spotify/", "deezer/", "musicbrainz/") so one service's entries can be dropped together: signing out of Spotify
// drops "spotify/" (Spotify Developer Policy §I.2).
//
// core/ may implement it over the database to survive restarts; MemoryMetaCache is the in-memory default.
//
// Threading: called only from the thread that owns the meta objects, and every call must return quickly (no disk
// or network round trip on that thread: a persistent implementation keeps a memory copy and writes behind).
class MetaCache
{
public:
    MetaCache() = default;
    MetaCache(const MetaCache &) = delete;
    MetaCache &operator=(const MetaCache &) = delete;
    MetaCache(MetaCache &&) = delete;
    MetaCache &operator=(MetaCache &&) = delete;
    virtual ~MetaCache();

    // The stored bytes, if present and not expired.
    virtual std::optional<QByteArray> get(const QString &key) = 0;
    // Stores (or replaces) an entry for `ttl`. A non-positive ttl stores nothing.
    virtual void put(const QString &key, const QByteArray &value, std::chrono::seconds ttl) = 0;
    // Drops every entry whose key starts with `prefix`.
    virtual void removePrefix(const QString &prefix) = 0;
};

// In-memory MetaCache: least recently used entries go first once `maxBytes` (keys + values) is exceeded; expired
// entries are dropped when touched. Not thread-safe (see MetaCache). The clock must outlive the cache.
class MemoryMetaCache final : public MetaCache
{
public:
    explicit MemoryMetaCache(const base::Clock &clock, qint64 maxBytes = qint64{32} * 1024 * 1024);

    std::optional<QByteArray> get(const QString &key) override;
    void put(const QString &key, const QByteArray &value, std::chrono::seconds ttl) override;
    void removePrefix(const QString &prefix) override;

    qint64 sizeBytes() const { return m_bytes; }
    qsizetype count() const { return m_index.size(); }

private:
    struct Entry
    {
        QString key;
        QByteArray value;
        base::Clock::TimePoint expires;
    };
    using Lru = std::list<Entry>; // most recently used first

    static qint64 cost(const Entry &entry);
    void erase(Lru::iterator it);

    const base::Clock &m_clock;
    qint64 m_maxBytes;
    qint64 m_bytes = 0;
    Lru m_lru;
    QHash<QString, Lru::iterator> m_index;
};

} // namespace selecta::meta
