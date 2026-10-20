// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchService.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <chrono>
#include <functional>
#include <optional>

class QTimer;

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// One saved search of the wishlist (PRD SRCH-14).
struct WishlistItem
{
    qint64 id = 0;
    QString text;         // as typed
    qint64 createdAt = 0; // Unix time, ms
    qint64 lastRunMs = 0; // 0: not run yet
    int found = 0;        // files its runs found that the user hasn't looked at yet

    friend bool operator==(const WishlistItem &, const WishlistItem &) = default;
};

// The wishlist (PRD SRCH-14, NET-6) on the engine thread: saved searches run again in the background, one at a time,
// one per interval the server sets (the server's WishlistSearch message, through the governor's budget, which keeps
// a reserve for searches typed by hand). Each run remembers the files it saw; files it hadn't seen before count as
// found, until the user looks.
//
// Threading: engine thread. Ownership: the database and the SearchService outlive it.
class WishlistService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;
    static constexpr int kMaxSeen = 20'000; // files remembered per search

    WishlistService(storage::Database &db, SearchService &search, WallClock now = {}, QObject *parent = nullptr);
    ~WishlistService() override;
    WishlistService(const WishlistService &) = delete;
    WishlistService &operator=(const WishlistService &) = delete;
    WishlistService(WishlistService &&) = delete;
    WishlistService &operator=(WishlistService &&) = delete;

    // Saves a search; the same search (same words, any order or case) is saved once. Returns its id; nullopt for
    // text with nothing to search.
    std::optional<qint64> add(const QString &text);
    void remove(qint64 id);
    // The user looked at what it found: nothing new until the next run finds more.
    void markSeen(qint64 id);
    // How often a wishlist search runs (the server's interval; NET-6).
    void setInterval(std::chrono::milliseconds interval);
    std::chrono::milliseconds interval() const;
    // Runs the search that waited longest now, unless one is running. For tests and "Search Now".
    void runNext();

    QList<WishlistItem> items();

Q_SIGNALS:
    void itemsChanged(const QList<selecta::core::WishlistItem> &items);
    // A run found files it hadn't seen.
    void found(const selecta::core::WishlistItem &item, int newFiles);

private:
    void finished(qint64 id, const SearchOutcome &outcome);
    void publish();

    storage::Database &m_db;
    SearchService &m_search;
    WallClock m_now;
    QTimer *m_timer = nullptr;
    quint64 m_running = 0; // the SearchService id of the run in flight
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::WishlistItem)
