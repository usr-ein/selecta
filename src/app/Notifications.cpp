// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Notifications.h"

#include "core/AlbumService.h"
#include "core/Albums.h"
#include "core/GetTracks.h"
#include "core/RoomService.h"
#include "core/SocialService.h"
#include "core/WishlistService.h"

#include <QDateTime>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

// Counts kept per conversation, room or wishlist entry while the user is away; a flood of new names stops adding.
constexpr qsizetype kMaxThreads = 1000;
constexpr int kMaxCount = 9999;

bool onItsWay(core::GetLine::DownloadState state)
{
    using State = core::GetLine::DownloadState;
    return state == State::Queued || state == State::Transferring || state == State::Converting;
}

QString trackName(const core::GetLine &line)
{
    const QString artist = line.release ? line.release->artist() : line.artist;
    const QString title = line.release ? line.release->title : line.title;
    if (title.isEmpty()) {
        return line.raw.trimmed();
    }
    return artist.isEmpty() ? title : artist + u" – "_s + title;
}

// At most `max` characters, never splitting a surrogate pair, with an ellipsis when cut.
QString shortened(const QString &text, qsizetype max)
{
    if (text.size() <= max) {
        return text;
    }
    qsizetype keep = max - 1;
    if (keep > 0 && text.at(keep - 1).isHighSurrogate()) {
        --keep;
    }
    return text.left(keep) + u'…';
}

// Control characters from the network become spaces; line breaks stay only where allowed.
QString cleaned(QString text, bool keepLineBreaks)
{
    for (QChar &c : text) {
        if (c.category() == QChar::Other_Control && !(keepLineBreaks && c == u'\n')) {
            c = u' ';
        }
    }
    return text.trimmed();
}

// Adds to a per-thread count, keeping at most kMaxThreads threads. Returns the new count.
template <typename Key>
int bump(QHash<Key, int> &counts, const Key &key, int by = 1)
{
    const auto found = counts.find(key);
    if (found == counts.end()) {
        const int count = std::clamp(by, 1, kMaxCount);
        if (counts.size() < kMaxThreads) {
            counts.insert(key, count);
        }
        return count;
    }
    *found = static_cast<int>(std::min<qint64>(qint64{*found} + by, kMaxCount));
    return *found;
}

} // namespace

// --- RecordingNotificationSink ---

void RecordingNotificationSink::post(const SystemNotification &notification)
{
    // Like Notification Center: the same id replaces the earlier one.
    std::erase_if(m_posted, [&notification](const SystemNotification &kept) { return kept.id == notification.id; });
    m_posted.push_back(notification);
    while (m_posted.size() > kKept) {
        m_posted.pop_front();
    }
}

QString RecordingNotificationSink::describe() const
{
    return u"kept for the debug socket (headless run), never shown"_s;
}

bool RecordingNotificationSink::click(const QString &id) const
{
    const auto found = std::find_if(m_posted.cbegin(), m_posted.cend(),
                                    [&id](const SystemNotification &kept) { return kept.id == id; });
    if (found == m_posted.cend()) {
        return false;
    }
    activate(found->target);
    return true;
}

#ifndef Q_OS_MACOS
// No system notifications off macOS yet: the window's own notifications are all there is.
std::unique_ptr<NotificationSink> createSystemNotificationSink()
{
    return nullptr;
}

bool runsInAppBundle()
{
    return false;
}
#endif

// --- GetTracksBatch ---

GetTracksBatch::Step GetTracksBatch::reset(const QList<core::GetLine> &lines)
{
    Step step;
    QSet<qint64> present;
    present.reserve(lines.size());
    for (const core::GetLine &line : lines) {
        present.insert(line.id);
        follow(line, step);
    }
    m_pending.removeIf([&present](qint64 id) { return !present.contains(id); }); // removed lines leave uncounted
    step.finished = finishIfDone();
    return step;
}

GetTracksBatch::Step GetTracksBatch::update(const core::GetLine &line)
{
    Step step;
    follow(line, step);
    step.finished = finishIfDone();
    return step;
}

void GetTracksBatch::follow(const core::GetLine &line, Step &step)
{
    if (onItsWay(line.downloadState)) {
        m_pending.insert(line.id); // joins the batch, or is still in it
        return;
    }
    if (!m_pending.remove(line.id)) {
        return; // not one this batch sent
    }
    using State = core::GetLine::DownloadState;
    switch (line.downloadState) {
    case State::Arrived:
        ++m_batch.arrived;
        ++step.arrived;
        m_lastSettled = trackName(line);
        break;
    case State::Failed:
        ++m_batch.failed;
        m_lastSettled = trackName(line);
        break;
    case State::None: // cancelled, or interrupted by a quit: it just leaves
    case State::Queued:
    case State::Transferring:
    case State::Converting:
        break;
    }
}

std::optional<FinishedBatch> GetTracksBatch::finishIfDone()
{
    if (!m_pending.isEmpty()) {
        return std::nullopt;
    }
    FinishedBatch finished = std::exchange(m_batch, {});
    const QString last = std::exchange(m_lastSettled, {});
    if (finished.arrived + finished.failed == 0) {
        return std::nullopt; // nothing on its way, or everything cancelled
    }
    if (finished.arrived + finished.failed == 1) {
        finished.track = last;
    }
    return finished;
}

// --- Notifier ---

Notifier::Notifier(NotificationSink *sink, AwayCheck away, QObject *parent)
    : QObject(parent)
    , m_sink(sink)
    , m_away(std::move(away))
    , m_run(QDateTime::currentMSecsSinceEpoch())
{
    if (m_sink != nullptr) {
        m_sink->setActivationHandler([this](const NotificationTarget &target) { Q_EMIT activated(target); });
    }
}

Notifier::~Notifier()
{
    if (m_sink != nullptr) {
        m_sink->setActivationHandler({});
    }
}

bool Notifier::isAway() const
{
    return m_away && m_away();
}

void Notifier::messageAdded(const core::ChatMessage &message)
{
    if (message.outgoing || message.peer.isEmpty() || !isAway()) {
        return; // with the user here, the window's notification is enough
    }
    const int count = bump(m_messagesAway, message.peer);
    SystemNotification notification;
    notification.id = u"message:"_s + message.peer; // the next message from them replaces it, with the count
    notification.thread = notification.id;
    notification.title =
        count == 1 ? tr("Message from %1").arg(message.peer) : tr("%1 messages from %2").arg(count).arg(message.peer);
    notification.body = message.text;
    notification.sound = count == 1;
    notification.target = {u"messages"_s, message.peer};
    post(std::move(notification));
}

void Notifier::roomLineAdded(const QString &room, const core::RoomLine &line)
{
    if (!line.mention || line.own || room.isEmpty() || !isAway()) {
        return;
    }
    const int count = bump(m_mentionsAway, room);
    SystemNotification notification;
    notification.id = u"room:"_s + room;
    notification.thread = notification.id;
    notification.title = count == 1 ? tr("%1 mentioned you in %2").arg(line.user, room)
                                    : tr("%1 mentions of you in %2").arg(count).arg(room);
    notification.body = count == 1 ? line.text : line.user + u": "_s + line.text;
    notification.sound = count == 1;
    notification.target = {u"rooms"_s, room};
    post(std::move(notification));
}

void Notifier::albumReported(const core::AlbumReport &report)
{
    // A report without an arrival tells of singles moved to the Trash on the user's word: they're here for that.
    if (!report.arrival || m_albumsArrived.contains(report.albumId)) {
        return;
    }
    m_albumsArrived.insert(report.albumId);
    if (!isAway()) {
        return;
    }
    countArrivals(1);
    SystemNotification notification;
    notification.id = u"album:%1:%2"_s.arg(m_run).arg(report.albumId);
    notification.thread = u"albums"_s;
    notification.title = report.complete ? tr("“%1” is in your Inbox").arg(report.album)
                                         : tr("“%1” arrived incomplete").arg(report.album);
    QStringList lines{report.tracks == 1 ? tr("1 track in Inbox/%1/").arg(report.where)
                                         : tr("%1 tracks in Inbox/%2/").arg(report.tracks).arg(report.where)};
    if (!report.warning.isEmpty()) {
        lines.append(report.warning);
    }
    notification.body = lines.join(u'\n');
    notification.target = {u"inbox"_s, {}};
    post(std::move(notification));
}

void Notifier::albumChanged(const core::AlbumDownload &album)
{
    // An arrival is told with its report (albumReported()); a failure has none.
    if (album.state != core::AlbumDownload::State::Failed || m_albumsFailed.contains(album.id)) {
        return;
    }
    m_albumsFailed.insert(album.id);
    if (!isAway()) {
        return;
    }
    const QString name =
        album.release && !album.release->album.isEmpty() ? album.release->album : album.folderName();
    SystemNotification notification;
    notification.id = u"album-failed:%1:%2"_s.arg(m_run).arg(album.id);
    notification.thread = u"albums"_s;
    notification.title = tr("Couldn't download “%1”").arg(name);
    notification.body = album.message;
    notification.target = {u"activity"_s, {}}; // its files and why they failed are in the transfer list
    post(std::move(notification));
}

void Notifier::wishlistFound(const core::WishlistItem &item, int newFiles)
{
    if (newFiles <= 0 || !isAway()) {
        return;
    }
    const int files = bump(m_wishlistAway, item.id, newFiles);
    SystemNotification notification;
    notification.id = u"wishlist:%1"_s.arg(item.id); // a later run for the same entry adds to it
    notification.thread = u"wishlist"_s;
    notification.title = files == 1 ? tr("Wishlist: 1 new file for “%1”").arg(item.text)
                                    : tr("Wishlist: %1 new files for “%2”").arg(files).arg(item.text);
    notification.target = {u"wishlist"_s, {}};
    post(std::move(notification));
}

void Notifier::getLinesReset(const QList<core::GetLine> &lines)
{
    finishBatch(m_batch.reset(lines));
}

void Notifier::getLineChanged(const core::GetLine &line)
{
    finishBatch(m_batch.update(line));
}

void Notifier::finishBatch(const GetTracksBatch::Step &step)
{
    if (step.arrived > 0 && isAway()) {
        countArrivals(step.arrived);
    }
    if (!step.finished) {
        return;
    }
    Q_EMIT getTracksFinished(*step.finished);
    if (isAway()) {
        SystemNotification notification = batchNotification(*step.finished);
        notification.id = u"get-tracks:%1:%2"_s.arg(m_run).arg(++m_serial);
        post(std::move(notification));
    }
}

SystemNotification Notifier::batchNotification(const FinishedBatch &batch)
{
    SystemNotification notification;
    notification.thread = u"get-tracks"_s;
    notification.title = tr("Get tracks finished");
    const bool named = batch.arrived + batch.failed == 1 && !batch.track.isEmpty();
    if (batch.failed == 0) {
        notification.body = named              ? tr("“%1” is in your Inbox.").arg(batch.track)
                            : batch.arrived == 1 ? tr("1 track is in your Inbox.")
                                                 : tr("%1 tracks are in your Inbox.").arg(batch.arrived);
    } else if (batch.arrived == 0) {
        notification.body = named               ? tr("“%1” couldn't be downloaded.").arg(batch.track)
                            : batch.failed == 1 ? tr("1 track couldn't be downloaded.")
                                                : tr("%1 tracks couldn't be downloaded.").arg(batch.failed);
    } else {
        const QString arrived =
            batch.arrived == 1 ? tr("1 track is in your Inbox") : tr("%1 tracks are in your Inbox").arg(batch.arrived);
        const QString failed =
            batch.failed == 1 ? tr("1 couldn't be downloaded.") : tr("%1 couldn't be downloaded.").arg(batch.failed);
        notification.body = arrived + u"; "_s + failed;
    }
    // What arrived is in the Inbox; when nothing did, the list says why.
    notification.target = {batch.arrived > 0 ? u"inbox"_s : u"get"_s, {}};
    return notification;
}

void Notifier::setUnreadMessages(int unread)
{
    unread = std::max(0, unread);
    if (unread != m_unread) {
        m_unread = unread;
        Q_EMIT badgeChanged(badge());
    }
}

void Notifier::userReturned()
{
    m_messagesAway.clear();
    m_mentionsAway.clear();
    m_wishlistAway.clear();
    if (m_arrivals != 0) {
        m_arrivals = 0;
        Q_EMIT badgeChanged(badge());
    }
}

void Notifier::countArrivals(int arrived)
{
    m_arrivals = static_cast<int>(std::min<qint64>(qint64{m_arrivals} + arrived, kMaxCount));
    Q_EMIT badgeChanged(badge());
}

void Notifier::post(SystemNotification notification)
{
    if (!m_enabled || m_sink == nullptr || !isAway()) {
        return; // turned off, nothing to show them with here, or the user is in Selecta
    }
    notification.title = shortened(cleaned(notification.title, false), kMaxTitle);
    notification.body = shortened(cleaned(notification.body, true), kMaxBody);
    m_sink->post(notification);
}

} // namespace selecta::app
