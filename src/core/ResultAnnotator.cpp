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
    , m_inbox(db)
    , m_corrections(db)
    , m_corrected(!m_corrections.isEmpty())
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
    correct(result);
    markQueued(result);
    settleUpgrade(result);
    describe(result);
}

void ResultAnnotator::correct(SearchResult &result)
{
    // Without a verdict anywhere, the matcher's ownership stands. With one, ownership is worked out again from what
    // the matcher found, every pass, so a verdict given since the last one applies at once.
    if (!m_corrected || result.candidates.isEmpty()) {
        return;
    }
    const TrackIdentity remote = remoteIdentity(result.parsed, result.durationSec());
    settleOwnership(result, [this, &remote](const OwnedCandidate &candidate) {
        return m_corrections.verdict(CorrectionKind::SearchResult, remote, identityOf(candidate));
    });
}

TrackIdentity ResultAnnotator::identityOf(const OwnedCandidate &candidate)
{
    const std::pair<quint64, int> key{candidate.id, static_cast<int>(candidate.source)};
    if (const auto found = m_identities.constFind(key); found != m_identities.cend()) {
        return *found;
    }
    TrackIdentity identity;
    if (candidate.source == match::Source::Inbox) {
        if (const std::optional<library::InboxItem> item = m_inbox.find(static_cast<qint64>(candidate.id))) {
            identity = inboxIdentity(*item);
        }
    } else if (const std::optional<CollectionFile> file = fileOf(candidate.id)) {
        identity = collectionIdentity(file->file);
    }
    m_identities.insert(key, identity);
    return identity;
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
