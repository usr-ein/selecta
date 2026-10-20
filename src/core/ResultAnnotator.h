// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/CollectionStore.h"
#include "core/SearchResults.h"
#include "library/HistoryStore.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

#include <optional>
#include <utility>

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}

namespace selecta::core {

// What only the engine knows about search results (PRD SRCH-11, FAIR-1, FAIR-4): files already in the transfers,
// downloads of this session, the owned file's name, and the peer's counters and back-off. One annotator per pass:
// database lookups are cached for its lifetime.
//
// Threading: the engine thread (it reads the database and the governor). Ownership: borrows both for its lifetime.
class ResultAnnotator
{
public:
    // The quality rank of what arrives in the Inbox: MP3 320 CBR (match/Quality.h).
    static constexpr int kInboxQualityRank = 4;

    ResultAnnotator(storage::Database &db, const governor::Governor &governor,
                    QSet<std::pair<QString, QByteArray>> queued, std::optional<qint64> session);

    void annotate(SearchResult &result);

private:
    void markQueued(SearchResult &result) const;
    void settleUpgrade(SearchResult &result);
    void describe(SearchResult &result);
    std::optional<CollectionFile> fileOf(quint64 id);
    std::optional<library::HistoryEntry> downloadOf(quint64 id);

    CollectionStore m_collection;
    library::HistoryStore m_history;
    const governor::Governor &m_governor;
    QSet<std::pair<QString, QByteArray>> m_queued; // (peer, path) of downloads not finished
    std::optional<qint64> m_session;
    QHash<quint64, std::optional<CollectionFile>> m_files;
    QHash<quint64, std::optional<library::HistoryEntry>> m_downloads;
};

} // namespace selecta::core
