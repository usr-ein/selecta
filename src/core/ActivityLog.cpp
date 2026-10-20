// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ActivityLog.h"

#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <optional>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcActivity, "selecta.core.activity")

using storage::Statement;

constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

bool isMetadataOperation(governor::Operation operation)
{
    switch (operation) {
    case governor::Operation::SpotifyRequest:
    case governor::Operation::MusicBrainzRequest:
    case governor::Operation::CoverArtRequest:
    case governor::Operation::DeezerRequest:
    case governor::Operation::YtDlpRun:
        return true;
    default:
        return false;
    }
}

// The peer requests we make ourselves: their allowed decision names the lookup that may follow.
bool isOwnPeerRequest(governor::Operation operation)
{
    switch (operation) {
    case governor::Operation::QueueDownload:
    case governor::Operation::PlaceInQueue:
    case governor::Operation::Browse:
    case governor::Operation::UserInfo:
    case governor::Operation::FolderContents:
        return true;
    default:
        return false;
    }
}

// Why we connect to a peer, as the end of "You looked up X's address to …".
QString lookupReason(slsk::ConnectPurpose purpose, std::optional<governor::Operation> asked)
{
    switch (purpose) {
    case slsk::ConnectPurpose::Request:
        switch (asked.value_or(governor::Operation::Login)) {
        case governor::Operation::Browse:
            return ActivityLog::tr("browse their shares");
        case governor::Operation::UserInfo:
            return ActivityLog::tr("ask for their user info");
        case governor::Operation::FolderContents:
            return ActivityLog::tr("ask what's in one of their folders");
        case governor::Operation::QueueDownload:
            return ActivityLog::tr("ask them to queue a download for you");
        case governor::Operation::PlaceInQueue:
            return ActivityLog::tr("ask where your download stands in their queue");
        default:
            return ActivityLog::tr("send them one of your requests");
        }
    case slsk::ConnectPurpose::SearchResponse:
        return ActivityLog::tr("send them your search results: they searched for something you share");
    case slsk::ConnectPurpose::Transfer:
        return ActivityLog::tr("carry on a file transfer between you (your download from them, or theirs from you)");
    case slsk::ConnectPurpose::Distributed:
        return ActivityLog::tr("try them as your parent in the distributed network, which passes other users' "
                               "searches on to you");
    }
    return ActivityLog::tr("connect to them");
}

// "You asked …" when the governor let it go, "You wanted to … Selecta held it back: why" when it didn't.
QString ownRequest(const ActivityEntry &entry, const QString &done, const QString &wanted)
{
    if (entry.outcome == u"allowed"_s) {
        return done;
    }
    return entry.explanation.isEmpty() ? ActivityLog::tr("%1. Selecta held it back.").arg(wanted)
                                       : ActivityLog::tr("%1. Selecta held it back: %2").arg(wanted, entry.explanation);
}

QString describeDownload(const ActivityEntry &e)
{
    const QString &peer = e.subject;
    if (e.outcome == u"wanted"_s) {
        return ActivityLog::tr("You want %1 from %2.").arg(e.explanation, peer);
    }
    if (e.outcome == u"completed"_s) {
        return ActivityLog::tr("You downloaded %1 from %2.").arg(e.explanation, peer);
    }
    if (e.outcome == u"failed"_s) {
        return ActivityLog::tr("Your download from %1 failed: %2").arg(peer, e.explanation);
    }
    if (e.outcome == u"from-preview"_s) {
        return ActivityLog::tr("Your preview of %1 from %2 became the download: nothing more to fetch.")
            .arg(e.explanation, peer);
    }
    if (e.outcome == u"kept-preview"_s) {
        return ActivityLog::tr("You kept the preview of %1 from %2: it carries on as a download.")
            .arg(e.explanation, peer);
    }
    return {};
}

QString describeAlbum(const ActivityEntry &e)
{
    const QString &peer = e.subject;
    if (e.outcome == u"wanted"_s) {
        return ActivityLog::tr("You want the album %1 from %2.").arg(e.explanation, peer);
    }
    if (e.outcome == u"failed"_s) {
        return ActivityLog::tr("Your album from %1 failed: %2").arg(peer, e.explanation);
    }
    if (e.outcome == u"cancelled"_s) {
        return ActivityLog::tr("You cancelled the album %1 from %2.").arg(e.explanation, peer);
    }
    if (e.outcome == u"arrived"_s) {
        return ActivityLog::tr("Your album from %1 is in your Inbox: %2").arg(peer, e.explanation);
    }
    return {};
}

QString describeArrival(const ActivityEntry &e)
{
    if (e.outcome == u"arrived"_s) {
        return ActivityLog::tr("Your file from %1 is in your Inbox: %2").arg(e.subject, e.explanation);
    }
    if (e.outcome == u"failed"_s) {
        return ActivityLog::tr("Your file from %1 couldn't go to your Inbox: %2").arg(e.subject, e.explanation);
    }
    return {};
}

// Uploads are always the peer's doing: they asked for the file.
QString describeUpload(const ActivityEntry &e)
{
    const QString &peer = e.subject;
    if (e.outcome == u"queued"_s) {
        return ActivityLog::tr("%1 asked to download %2 from you; it waits in your upload queue.")
            .arg(peer, e.explanation);
    }
    if (e.outcome == u"started"_s) {
        return ActivityLog::tr("%1 is downloading %2 from you.").arg(peer, e.explanation);
    }
    if (e.outcome == u"completed"_s) {
        return ActivityLog::tr("%1 downloaded %2 from you.").arg(peer, e.explanation);
    }
    if (e.outcome == u"cancelled"_s) {
        return ActivityLog::tr("%1's download of %2 from you was cancelled.").arg(peer, e.explanation);
    }
    if (e.outcome == u"declined"_s) {
        return ActivityLog::tr("%1 no longer wanted %2 from you.").arg(peer, e.explanation);
    }
    if (e.outcome == u"failed"_s) {
        return ActivityLog::tr("%1's download of %2 from you failed.").arg(peer, e.explanation);
    }
    if (e.outcome == u"denied"_s) {
        return ActivityLog::tr("%1 asked to download from you; Selecta said no: %2").arg(peer, e.explanation);
    }
    return {};
}

QString describeUserList(const ActivityEntry &e)
{
    if (e.outcome == u"buddy"_s) {
        return ActivityLog::tr("You added %1 to your buddies.").arg(e.subject);
    }
    if (e.outcome == u"ignored"_s) {
        return ActivityLog::tr("You ignore %1 now: their messages are dropped.").arg(e.subject);
    }
    if (e.outcome == u"banned"_s) {
        return ActivityLog::tr("You banned %1: they can't browse or download from you.").arg(e.subject);
    }
    if (e.outcome == u"unlisted"_s) {
        return ActivityLog::tr("You took %1 off your lists.").arg(e.subject);
    }
    return {};
}

QString describePeerLookup(const ActivityEntry &e)
{
    // Lines written before lookups carried their purpose hold the governor's own text ("Peer lookups: allowed.").
    if (!e.explanation.startsWith(u"Peer lookups"_s)) {
        return e.explanation;
    }
    return ownRequest(e,
                      ActivityLog::tr("You looked up %1's address to connect to them (this line is older than "
                                      "Selecta's notes on why).")
                          .arg(e.subject),
                      ActivityLog::tr("You wanted to look up %1's address").arg(e.subject));
}

} // namespace

ActivityLog::ActivityLog(storage::Database &db, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{}

void ActivityLog::recordDecision(governor::Operation operation, const QString &subject, governor::Refusal refusal,
                                 const QString &explanation)
{
    // A metadata request that goes ahead is logged once it finishes (GovernorPacer::report), with its outcome; only
    // the refusals are worth a line of their own.
    if (refusal == governor::Refusal::None && isMetadataOperation(operation)) {
        return;
    }
    // A peer lookup is logged with its purpose, which the governor doesn't know (recordPeerLookup()).
    if (operation == governor::Operation::PeerLookup) {
        return;
    }
    if (refusal == governor::Refusal::None && isOwnPeerRequest(operation) && !subject.isEmpty()) {
        remember(subject, operation);
    }
    record(governor::operationName(operation), subject,
           refusal == governor::Refusal::None ? u"allowed"_s : governor::refusalName(refusal), explanation);
}

void ActivityLog::recordPeerLookup(const QString &username, slsk::ConnectPurpose purpose, governor::Refusal refusal,
                                   const QString &explanation)
{
    std::optional<governor::Operation> asked;
    if (purpose == slsk::ConnectPurpose::Request) {
        if (const auto it = m_lastAsked.constFind(username);
            it != m_lastAsked.constEnd() && m_now() - it->atMs <= kRequestMemoryMs) {
            asked = it->operation;
        }
    }
    const QString reason = lookupReason(purpose, asked);
    const bool allowed = refusal == governor::Refusal::None;
    QString sentence;
    if (allowed) {
        sentence = tr("You looked up %1's address to %2.").arg(username, reason);
    } else if (explanation.isEmpty()) {
        sentence = tr("You wanted to look up %1's address to %2. Selecta held it back.").arg(username, reason);
    } else {
        sentence =
            tr("You wanted to look up %1's address to %2. Selecta held it back: %3").arg(username, reason, explanation);
    }
    record(governor::operationName(governor::Operation::PeerLookup), username,
           allowed ? u"allowed"_s : governor::refusalName(refusal), sentence);
}

void ActivityLog::remember(const QString &peer, governor::Operation operation)
{
    const qint64 now = m_now();
    if (m_lastAsked.size() >= kRequestMemoryPeers && !m_lastAsked.contains(peer)) {
        // Forget what's too old to name a lookup; if every one is recent (a burst), start over.
        m_lastAsked.removeIf([now](decltype(m_lastAsked)::iterator it) { return now - it->atMs > kRequestMemoryMs; });
        if (m_lastAsked.size() >= kRequestMemoryPeers) {
            m_lastAsked.clear();
        }
    }
    m_lastAsked.insert(peer, AskedOf{.operation = operation, .atMs = now});
}

QString ActivityLog::describe(const ActivityEntry &entry)
{
    const QString &op = entry.operation;
    const QString &peer = entry.subject;
    QString sentence;
    if (peer.isEmpty()) {
        return entry.explanation;
    }
    if (op == u"peer-lookup"_s) {
        sentence = describePeerLookup(entry);
    } else if (op == u"browse"_s) {
        sentence = ownRequest(entry, tr("You asked to browse %1's shares.").arg(peer),
                              tr("You wanted to browse %1's shares").arg(peer));
    } else if (op == u"user-info"_s) {
        sentence = ownRequest(entry, tr("You asked %1 for their user info.").arg(peer),
                              tr("You wanted to ask %1 for their user info").arg(peer));
    } else if (op == u"folder-contents"_s) {
        sentence = ownRequest(entry, tr("You asked %1 what's in one of their folders.").arg(peer),
                              tr("You wanted to ask %1 what's in one of their folders").arg(peer));
    } else if (op == u"queue-download"_s) {
        sentence = ownRequest(entry, tr("You asked %1 to queue a download for you.").arg(peer),
                              tr("You wanted to ask %1 to queue a download for you").arg(peer));
    } else if (op == u"place-in-queue"_s) {
        sentence = ownRequest(entry, tr("You asked %1 where your download stands in their queue.").arg(peer),
                              tr("You wanted to ask %1 where your download stands in their queue").arg(peer));
    } else if (op == u"private-message"_s) {
        sentence = ownRequest(entry, tr("You sent %1 a private message.").arg(peer),
                              tr("You wanted to send %1 a private message").arg(peer));
    } else if (op == u"download"_s) {
        sentence = describeDownload(entry);
    } else if (op == u"album"_s) {
        sentence = describeAlbum(entry);
    } else if (op == u"arrival"_s) {
        sentence = describeArrival(entry);
    } else if (op == u"upload"_s) {
        sentence = describeUpload(entry);
    } else if (op == u"shares"_s && entry.outcome == u"browsed"_s) {
        sentence = tr("%1 is browsing your shares: they asked for the list of what you share.").arg(peer);
    } else if (op == u"message"_s && entry.outcome == u"ignored"_s) {
        sentence = tr("%1 sent you a private message; you ignore them, so Selecta dropped it.").arg(peer);
    } else if (op == u"user-list"_s) {
        sentence = describeUserList(entry);
    }
    return sentence.isEmpty() ? entry.explanation : sentence;
}

ActivityEntry ActivityLog::record(const QString &operation, const QString &subject, const QString &outcome,
                                  const QString &explanation)
{
    ActivityEntry entry{.id = 0,
                        .atMs = m_now(),
                        .operation = operation,
                        .subject = subject,
                        .outcome = outcome,
                        .explanation = explanation};
    Statement insert = m_db.prepare(
        u"INSERT INTO activity (at, operation, subject, outcome, explanation) VALUES (?1, ?2, ?3, ?4, ?5);");
    if (!insert.isValid() || !insert.bind(1, entry.atMs)
                                  .bind(2, entry.operation)
                                  .bind(3, entry.subject)
                                  .bind(4, entry.outcome)
                                  .bind(5, entry.explanation)
                                  .run()) {
        qCWarning(lcActivity) << "couldn't record activity:" << m_db.lastError();
        return entry;
    }
    entry.id = m_db.lastInsertRowId();
    Q_EMIT recorded(entry);
    return entry;
}

int ActivityLog::prune()
{
    storage::Transaction transaction(m_db);
    if (!transaction.isActive()) {
        return -1;
    }
    Statement byAge = m_db.prepare(u"DELETE FROM activity WHERE at < ?1;");
    if (!byAge.isValid() || !byAge.bind(1, m_now() - (kKeepDays * kDayMs)).run()) {
        return -1;
    }
    int removed = m_db.changes();
    // Keep the newest kMaxRows by id (ids only grow).
    Statement byCount =
        m_db.prepare(u"DELETE FROM activity WHERE id <= (SELECT id FROM activity ORDER BY id DESC LIMIT 1 OFFSET ?1);");
    if (!byCount.isValid() || !byCount.bind(1, kMaxRows).run()) {
        return -1;
    }
    removed += m_db.changes();
    if (!transaction.commit()) {
        return -1;
    }
    return removed;
}

QList<ActivityEntry> ActivityLog::recent(storage::Database &db, int limit)
{
    QList<ActivityEntry> entries;
    Statement query =
        db.prepare(u"SELECT id, at, operation, subject, outcome, explanation FROM activity ORDER BY id DESC LIMIT ?1;");
    if (!query.isValid()) {
        return entries;
    }
    query.bind(1, std::max(0, limit));
    while (query.step() == Statement::Step::Row) {
        entries.append(ActivityEntry{.id = query.int64(0),
                                     .atMs = query.int64(1),
                                     .operation = query.text(2),
                                     .subject = query.text(3),
                                     .outcome = query.text(4),
                                     .explanation = query.text(5)});
    }
    return entries;
}

} // namespace selecta::core
