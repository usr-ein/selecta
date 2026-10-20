// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/CollectionStore.h"
#include "core/MatchCorrections.h"
#include "core/SearchResults.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"

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

// What only the engine knows about search results (PRD SRCH-11, FAIR-1, FAIR-4, LIB-6): the user's corrections of the
// matches, files already in the transfers, downloads of this session, the owned file's name, and the peer's counters
// and back-off. One annotator per pass: database lookups are cached for its lifetime.
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

    // LIB-6: remembers the user's verdict on a result and the owned track it shows as matched (ownedId, ownedSource):
    // "Not What I Have" (Different) or "Same as What I Have" (Same). False when the result owns nothing, either track
    // can't be told apart (an unparseable name, a record gone), or the database fails.
    static bool recordCorrection(storage::Database &db, const SearchResult &result, Verdict verdict, qint64 atMs);

private:
    void correct(SearchResult &result);
    TrackIdentity identityOf(const OwnedCandidate &candidate);
    void markQueued(SearchResult &result) const;
    void settleUpgrade(SearchResult &result);
    void describe(SearchResult &result);
    std::optional<CollectionFile> fileOf(quint64 id);
    std::optional<library::HistoryEntry> downloadOf(quint64 id);

    CollectionStore m_collection;
    library::HistoryStore m_history;
    library::InboxStore m_inbox;
    MatchCorrectionStore m_corrections;
    bool m_corrected = false; // the user corrected some match: worth looking verdicts up
    const governor::Governor &m_governor;
    QSet<std::pair<QString, QByteArray>> m_queued; // (peer, path) of downloads not finished
    std::optional<qint64> m_session;
    QHash<quint64, std::optional<CollectionFile>> m_files;
    QHash<quint64, std::optional<library::HistoryEntry>> m_downloads;
    QHash<std::pair<quint64, int>, TrackIdentity> m_identities; // (id, match::Source) → the owned track's identity
};

} // namespace selecta::core
