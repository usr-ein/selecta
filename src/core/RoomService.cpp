// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RoomService.h"

#include "core/ActivityLog.h"
#include "core/NetworkService.h"
#include "core/SocialService.h"
#include "governor/Governor.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcRooms, "selecta.core.rooms")

using storage::Statement;

constexpr int kFlushMs = 300;

// Whether `text` names `user` as a word of its own.
bool names(const QString &text, const QString &user)
{
    if (user.isEmpty()) {
        return false;
    }
    qsizetype from = 0;
    while ((from = text.indexOf(user, from, Qt::CaseInsensitive)) >= 0) {
        const qsizetype end = from + user.size();
        const bool startsAWord = from == 0 || !text.at(from - 1).isLetterOrNumber();
        const bool endsAWord = end >= text.size() || !text.at(end).isLetterOrNumber();
        if (startsAWord && endsAWord) {
            return true;
        }
        from = end;
    }
    return false;
}

QString chatProblem(slsk::ChatTextError error)
{
    switch (error) {
    case slsk::ChatTextError::None:
        return {};
    case slsk::ChatTextError::Empty:
        return RoomService::tr("Nothing to send.");
    case slsk::ChatTextError::TooLong:
        return RoomService::tr("That line is too long for Soulseek.");
    case slsk::ChatTextError::InvalidRoomName:
        return RoomService::tr("Room names are 1 to 24 plain letters, digits or signs, with no space at either end and "
                               "never two in a row.");
    }
    return {};
}

bool beforeCaseless(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) < 0;
}

} // namespace

RoomService::RoomService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                         SocialService &social, ActivityLog &activity, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_network(network)
    , m_social(social)
    , m_activity(activity)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_joinTimer(new QTimer(this))
    , m_flushTimer(new QTimer(this))
{
    qRegisterMetaType<RoomListing>();
    qRegisterMetaType<QList<RoomListing>>();
    qRegisterMetaType<RoomLine>();
    qRegisterMetaType<Room>();
    m_joinTimer->setSingleShot(true);
    connect(m_joinTimer, &QTimer::timeout, this, &RoomService::pumpJoins);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(kFlushMs);
    connect(m_flushTimer, &QTimer::timeout, this, &RoomService::flush);

    // The rooms open at quit wait for the next login.
    Statement query = m_db.prepare(u"SELECT name FROM joined_rooms ORDER BY joined_ms LIMIT ?1;");
    if (query.isValid()) {
        query.bind(1, kMaxRooms);
        while (query.step() == Statement::Step::Row) {
            const QString name = query.text(0);
            m_rooms.insert(name, Room{.name = name});
        }
    }

    const slsk::ServerSession &server = m_network.server();
    connect(&server, &slsk::ServerSession::loggedIn, this, &RoomService::loggedIn);
    connect(&server, &slsk::ServerSession::stateChanged, this, [this](slsk::ServerSession::State state) {
        if (state != slsk::ServerSession::State::LoggedIn) {
            wentOffline();
        }
    });
    connect(&server, &slsk::ServerSession::roomListReceived, this, &RoomService::listReceived);
    connect(&server, &slsk::ServerSession::roomJoined, this, &RoomService::joined);
    connect(&server, &slsk::ServerSession::roomJoinFailed, this, &RoomService::joinFailed);
    connect(&server, &slsk::ServerSession::roomLeft, this, &RoomService::left);
    connect(&server, &slsk::ServerSession::roomUserJoined, this, &RoomService::userJoined);
    connect(&server, &slsk::ServerSession::roomUserLeft, this, &RoomService::userLeft);
    connect(&server, &slsk::ServerSession::roomMessageReceived, this, &RoomService::lineReceived);
    if (m_network.isOnline()) {
        loggedIn();
    }
}

void RoomService::refreshList()
{
    if (!m_network.isOnline()) {
        return;
    }
    if (m_lastList.isValid() && m_lastList.elapsed() < kListInterval.count()) {
        Q_EMIT listChanged(m_list); // the list we have is recent enough
        return;
    }
    if (m_network.server().requestRoomList()) {
        m_lastList.start();
        m_activity.record(u"room-list"_s, QString(), u"sent"_s, QString());
    }
}

QString RoomService::join(const QString &room)
{
    if (const QString problem = chatProblem(slsk::validateRoomName(room)); !problem.isEmpty()) {
        return problem;
    }
    const auto found = m_rooms.constFind(room);
    if (found != m_rooms.constEnd() && found->state != Room::State::Failed) {
        return {}; // in it, or on the way
    }
    if (!m_network.isOnline()) {
        return tr("Not connected to Soulseek.");
    }
    if (found == m_rooms.constEnd() && m_rooms.size() >= kMaxRooms) {
        return tr("You're in %1 rooms already: leave one first.").arg(kMaxRooms);
    }
    m_rooms.insert(room, Room{.name = room});
    remember(room, true);
    m_joinQueue.append(room);
    changed(room);
    pumpJoins();
    return {};
}

void RoomService::leave(const QString &room)
{
    const auto it = m_rooms.find(room);
    if (it == m_rooms.end()) {
        return;
    }
    if (it->state == Room::State::Joining || it->state == Room::State::Joined) {
        m_network.server().leaveRoom(room);
    }
    m_rooms.erase(it);
    m_joinQueue.removeAll(room);
    m_lines.remove(room);
    m_dirty.remove(room);
    remember(room, false);
    m_activity.record(u"room"_s, room, u"left"_s, QString());
    Q_EMIT roomClosed(room);
}

QString RoomService::say(const QString &room, const QString &text)
{
    const auto it = m_rooms.constFind(room);
    if (it == m_rooms.constEnd() || it->state != Room::State::Joined) {
        return m_network.isOnline() ? tr("You're not in this room yet.") : tr("Not connected to Soulseek.");
    }
    QString line = text.trimmed();
    if (const QString problem = chatProblem(slsk::prepareChatLine(line)); !problem.isEmpty()) {
        return problem;
    }
    // Lines are paced with private messages, and never repeated in a burst (SAFE-1).
    governor::Decision decision =
        m_governor.request(governor::Operation::RoomMessage, {.recipient = room, .text = line});
    if (!decision.permit) {
        return decision.explanation;
    }
    if (!m_network.server().sayInRoom(room, line, std::move(*decision.permit))) {
        return tr("The line couldn't be sent.");
    }
    return {};
}

void RoomService::setViewing(const QString &room)
{
    m_viewing = room;
    const auto it = m_rooms.find(room);
    if (it != m_rooms.end() && (it->unread > 0 || it->mentions > 0)) {
        it->unread = 0;
        it->mentions = 0;
        changed(room);
    }
}

QList<Room> RoomService::rooms() const
{
    QList<Room> rooms = m_rooms.values();
    std::ranges::sort(rooms, [](const Room &a, const Room &b) { return beforeCaseless(a.name, b.name); });
    return rooms;
}

void RoomService::loggedIn()
{
    // Every room we were in is joined again, one at a time.
    m_joinQueue.clear();
    QStringList names = m_rooms.keys();
    std::ranges::sort(names, beforeCaseless);
    for (const QString &name : std::as_const(names)) {
        Room &room = m_rooms[name];
        if (room.state != Room::State::Failed) {
            room.state = Room::State::Waiting;
            m_joinQueue.append(name);
            changed(name);
        }
    }
    pumpJoins();
}

void RoomService::wentOffline()
{
    m_joinQueue.clear();
    m_joinTimer->stop();
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        if (it->state == Room::State::Joining || it->state == Room::State::Joined) {
            it->state = Room::State::Waiting;
            it->members.clear();
            changed(it.key());
        }
    }
}

void RoomService::pumpJoins()
{
    if (!m_network.isOnline()) {
        return;
    }
    while (!m_joinQueue.isEmpty()) {
        // One join every 2 s at most (references/soulseek-protocol.md §12.7).
        if (m_lastJoin.isValid() && m_lastJoin.elapsed() < kJoinSpacing.count()) {
            m_joinTimer->start(static_cast<int>(kJoinSpacing.count() - m_lastJoin.elapsed()));
            return;
        }
        const QString name = m_joinQueue.takeFirst();
        const auto room = m_rooms.find(name);
        if (room == m_rooms.end() || room->state != Room::State::Waiting) {
            continue;
        }
        m_lastJoin.start();
        if (!m_network.server().joinRoom(name)) {
            room->state = Room::State::Failed;
            room->message = tr("The server didn't take the request.");
            changed(name);
            continue;
        }
        room->state = Room::State::Joining;
        m_activity.record(u"room"_s, name, u"joining"_s, QString());
        changed(name);
    }
}

void RoomService::listReceived(const slsk::fromserver::RoomList &list)
{
    // Public rooms only: private rooms are out of scope (D32).
    m_list.clear();
    m_list.reserve(list.publicRooms.size());
    for (const slsk::RoomListEntry &entry : list.publicRooms) {
        m_list.append(RoomListing{.name = entry.name, .users = static_cast<int>(entry.userCount)});
    }
    std::ranges::sort(m_list, [](const RoomListing &a, const RoomListing &b) {
        return a.users != b.users ? a.users > b.users : beforeCaseless(a.name, b.name);
    });
    Q_EMIT listChanged(m_list);
}

void RoomService::joined(const slsk::fromserver::RoomJoined &room)
{
    const auto it = m_rooms.find(room.room);
    if (it == m_rooms.end()) {
        return; // left meanwhile: the session leaves rooms nobody asked for
    }
    it->state = Room::State::Joined;
    it->message.clear();
    it->members.clear();
    it->members.reserve(room.members.size());
    for (const slsk::RoomMember &member : room.members) {
        it->members.append(member.username);
    }
    std::ranges::sort(it->members, beforeCaseless);
    m_activity.record(u"room"_s, room.room, u"joined"_s, QString());
    changed(room.room);
}

void RoomService::joinFailed(const QString &room)
{
    const auto it = m_rooms.find(room);
    if (it == m_rooms.end()) {
        return;
    }
    it->state = Room::State::Failed;
    it->message = tr("“%1” is a private room. Selecta joins public rooms only.").arg(room);
    remember(room, false);
    changed(room);
}

void RoomService::left(const QString &room)
{
    // Our own leaving is already done; anything else is the server taking us out.
    const auto it = m_rooms.find(room);
    if (it == m_rooms.end() || it->state != Room::State::Joined) {
        return;
    }
    qCInfo(lcRooms) << "the server took us out of" << room;
    m_rooms.erase(it);
    m_lines.remove(room);
    m_dirty.remove(room);
    remember(room, false);
    Q_EMIT roomClosed(room);
}

void RoomService::userJoined(const slsk::fromserver::RoomUserJoined &event)
{
    const auto it = m_rooms.find(event.room);
    if (it == m_rooms.end() || it->state != Room::State::Joined) {
        return; // our own arrival comes before the member list: the list has us
    }
    const auto at = std::ranges::lower_bound(it->members, event.member.username, beforeCaseless);
    if (at == it->members.end() || *at != event.member.username) {
        it->members.insert(at, event.member.username);
        changed(event.room);
    }
}

void RoomService::userLeft(const slsk::fromserver::RoomUserLeft &event)
{
    const auto it = m_rooms.find(event.room);
    if (it != m_rooms.end() && it->members.removeOne(event.username)) {
        changed(event.room);
    }
}

void RoomService::lineReceived(const slsk::fromserver::RoomMessage &message)
{
    const auto it = m_rooms.find(message.room);
    if (it == m_rooms.end() || it->state != Room::State::Joined ||
        m_social.listOf(message.username) == UserList::Ignored) {
        return;
    }
    const QString self = m_network.username();
    RoomLine line{.atMs = m_now(),
                  .user = message.username,
                  .text = message.message.left(kShownLineMax),
                  .own = message.username == self,
                  .mention = false};
    line.mention = !line.own && names(line.text, self);
    QList<RoomLine> &lines = m_lines[message.room];
    lines.append(line);
    if (lines.size() > kKeptLines) {
        lines.remove(0, lines.size() - kKeptLines);
    }
    if (!line.own && message.room != m_viewing) {
        ++it->unread;
        it->mentions += line.mention ? 1 : 0;
        changed(message.room);
    }
    Q_EMIT lineAdded(message.room, line);
}

void RoomService::changed(const QString &room)
{
    m_dirty.insert(room);
    if (!m_flushTimer->isActive()) {
        m_flushTimer->start();
    }
}

void RoomService::flush()
{
    const QSet<QString> dirty = std::exchange(m_dirty, {});
    for (const QString &name : dirty) {
        if (const auto it = m_rooms.constFind(name); it != m_rooms.constEnd()) {
            Q_EMIT roomChanged(*it);
        }
    }
}

void RoomService::remember(const QString &room, bool open)
{
    Statement statement = open ? m_db.prepare(u"INSERT OR REPLACE INTO joined_rooms (name, joined_ms) VALUES (?1, ?2);")
                               : m_db.prepare(u"DELETE FROM joined_rooms WHERE name = ?1;");
    if (!statement.isValid()) {
        return;
    }
    statement.bind(1, room);
    if (open) {
        statement.bind(2, m_now());
    }
    if (!statement.run()) {
        qCWarning(lcRooms) << "couldn't remember the room" << room << ':' << m_db.lastError();
    }
}

} // namespace selecta::core
