// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ResultAnnotator.h"

#include "governor/Governor.h"
#include "match/Quality.h"

#include <QCoreApplication>

#include <chrono>

namespace selecta::core {

ResultAnnotator::ResultAnnotator(storage::Database &db, const governor::Governor &governor,
                                 QSet<std::pair<QString, QByteArray>> queued, std::optional<qint64> session)
    : m_collection(db)
    , m_history(db)
    , m_governor(governor)
    , m_queued(std::move(queued))
    , m_session(session)
{}

void ResultAnnotator::annotate(SearchResult &result)
{
    result.peerFilesToday = m_governor.peerToday(result.hit.peer).files;
    const std::optional<base::Clock::Duration> backoff = m_governor.peerBackoff(result.hit.peer);
    result.peerBackoffMs =
        backoff ? std::chrono::duration_cast<std::chrono::milliseconds>(*backoff).count() : qint64{0};
    markQueued(result);
    settleUpgrade(result);
    describe(result);
}

void ResultAnnotator::markQueued(SearchResult &result) const
{
    // A file in the transfers says so, unless something closer to home is known about it.
    const bool inTransfers = m_queued.contains({result.hit.peer, result.hit.path});
    if (result.ownership == Ownership::Queued && !inTransfers) {
        result.ownership = result.historyIds.isEmpty() ? Ownership::None : Ownership::DownloadedBefore;
    }
    const bool lesser = result.ownership == Ownership::None || result.ownership == Ownership::OtherVersion ||
                        result.ownership == Ownership::DownloadedBefore ||
                        result.ownership == Ownership::DownloadedThisSession;
    if (inTransfers && lesser) {
        result.ownership = Ownership::Queued;
    }
}

void ResultAnnotator::settleUpgrade(SearchResult &result)
{
    if (result.ownership != Ownership::Upgrade) {
        return;
    }
    // Every download arrives as an MP3 320 (D29): over a copy that good already, nothing would improve.
    const std::optional<CollectionFile> owned = fileOf(result.ownedId);
    if (owned && match::qualityRank(owned->file.quality) >= kInboxQualityRank) {
        result.ownership = Ownership::InCollection;
    }
}

void ResultAnnotator::describe(SearchResult &result)
{
    result.ownershipDetail.clear();
    switch (result.ownership) {
    case Ownership::InCollection:
    case Ownership::Upgrade:
    case Ownership::LikelyInCollection:
        if (const std::optional<CollectionFile> owned = fileOf(result.ownedId)) {
            result.ownershipDetail = owned->file.file.path;
        }
        break;
    case Ownership::OtherVersion:
        if (result.ownedSource != match::Source::Collection) {
            result.ownershipDetail = QCoreApplication::translate("selecta::core::ResultAnnotator", "in your Inbox");
        } else if (const std::optional<CollectionFile> owned = fileOf(result.ownedId)) {
            result.ownershipDetail = owned->file.file.path;
        }
        break;
    case Ownership::DownloadedBefore:
    case Ownership::DownloadedThisSession:
        for (const quint64 id : std::as_const(result.historyIds)) {
            const std::optional<library::HistoryEntry> entry = downloadOf(id);
            if (!entry) {
                continue;
            }
            const bool thisSession = m_session && entry->session && *entry->session == *m_session;
            result.ownership = thisSession ? Ownership::DownloadedThisSession : Ownership::DownloadedBefore;
            result.ownershipDetail =
                entry->whereabouts == library::HistoryEntry::Whereabouts::Filed
                    ? entry->filedPath
                    : QCoreApplication::translate("selecta::core::ResultAnnotator", "from %1").arg(entry->peer);
            break;
        }
        break;
    default:
        break;
    }
}

std::optional<CollectionFile> ResultAnnotator::fileOf(quint64 id)
{
    auto found = m_files.find(id);
    if (found == m_files.end()) {
        found = m_files.insert(id, m_collection.find(static_cast<qint64>(id)));
    }
    return *found;
}

std::optional<library::HistoryEntry> ResultAnnotator::downloadOf(quint64 id)
{
    auto found = m_downloads.find(id);
    if (found == m_downloads.end()) {
        found = m_downloads.insert(id, m_history.find(static_cast<qint64>(id)));
    }
    return *found;
}

} // namespace selecta::core
