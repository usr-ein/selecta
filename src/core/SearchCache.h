// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchService.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// Finished searches kept in the database for reuse (PRD SRCH-12, CACHE-1): a query run again within the lifetime
// (a day by default) is answered from here, labelled with its age, without spending search budget. Only searches that
// found something are kept: an empty answer is worth asking again later, and the governor keeps an identical query
// quiet for a while anyway.
//
// Threading: the thread that owns the database connection (the engine thread). Failure modes: a database error or a
// damaged entry reads as a miss (a damaged entry is deleted); store() reports failure through its return value.
class SearchCacheStore
{
public:
    using WallClock = std::function<qint64()>;

    static constexpr qint64 kDefaultLifetimeMs = qint64{24} * 60 * 60 * 1000;
    static constexpr int kMaxSearches = 500; // the oldest go first beyond this
    // Bounds for decoding what the database holds (a damaged or foreign blob must not allocate without limit).
    static constexpr qsizetype kMaxHits = 50'000;
    static constexpr qsizetype kMaxEncodedBytes = qsizetype{64} * 1024 * 1024;

    struct Entry
    {
        SearchOutcome outcome; // fromCache set, searchedAtMs from when the search ran
    };

    explicit SearchCacheStore(storage::Database &db, WallClock now = {}, qint64 lifetimeMs = kDefaultLifetimeMs);

    // The search kept under `key` (governor::queryKey() of the query as sent), if it is younger than the lifetime.
    std::optional<Entry> find(const QString &key);
    // Keeps a finished search under `key`, replacing an older one, then drops expired searches and the oldest beyond
    // kMaxSearches. An outcome without hits isn't kept (returns true: nothing to do).
    bool store(const QString &key, const SearchOutcome &outcome);

    // Settings → Storage (CACHE-2).
    int count();
    qint64 sizeBytes();
    bool clear();
    qint64 lifetimeMs() const { return m_lifetimeMs; }
    void setLifetimeMs(qint64 lifetimeMs) { m_lifetimeMs = lifetimeMs; }

    // The blob format, public for tests: versioned, compressed, bounded on decoding.
    static QByteArray encode(const QList<SearchHit> &hits);
    static std::optional<QList<SearchHit>> decode(const QByteArray &blob);

private:
    void prune();

    storage::Database &m_db;
    WallClock m_now;
    qint64 m_lifetimeMs;
};

} // namespace selecta::core
