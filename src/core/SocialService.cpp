// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SocialService.h"

#include "core/ActivityLog.h"
#include "core/NetworkService.h"
#include "governor/Governor.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcSocial, "selecta.core.social")

using storage::Statement;

constexpr std::array kLists{UserList::Buddy, UserList::Ignored, UserList::Banned};

std::optional<UserList> listFrom(const QString &name)
{
    for (const UserList list : kLists) {
        if (userListName(list) == name) {
            return list;
        }
    }
    return std::nullopt;
}

Presence presenceOf(slsk::UserStatus status)
{
    switch (status) {
    case slsk::UserStatus::Online:
        return Presence::Online;
    case slsk::UserStatus::Away:
        return Presence::Away;
    case slsk::UserStatus::Offline:
        break;
    }
    return Presence::Offline;
}

ChatMessage readMessage(const Statement &row)
{
    return ChatMessage{.id = row.int64(0),
                       .peer = row.text(1),
                       .atMs = row.int64(2),
                       .outgoing = row.int64(3) != 0,
                       .text = row.text(4),
                       .read = row.int64(5) != 0};
}

} // namespace

QString userListName(UserList list)
{
    switch (list) {
    case UserList::Buddy:
        return u"buddy"_s;
    case UserList::Ignored:
        return u"ignored"_s;
    case UserList::Banned:
        return u"banned"_s;
    }
    return u"buddy"_s;
}

SocialService::SocialService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                             ActivityLog &activity, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_network(network)
    , m_activity(activity)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
    qRegisterMetaType<ChatMessage>();
    qRegisterMetaType<Conversation>();
    qRegisterMetaType<QList<Conversation>>();
    qRegisterMetaType<ListedUser>();
    qRegisterMetaType<QList<ListedUser>>();
    qRegisterMetaType<QSet<QString>>();
    connect(&m_network, &NetworkService::privateMessageReceived, this, &SocialService::received);
    const slsk::ServerSession &server = m_network.server();
    connect(&server, &slsk::ServerSession::loggedIn, this, &SocialService::watchBuddies);
    connect(&server, &slsk::ServerSession::stateChanged, this, [this](slsk::ServerSession::State state) {
        if (state != slsk::ServerSession::State::LoggedIn && !m_presence.isEmpty()) {
            m_presence.clear(); // nothing is known while we're offline
            Q_EMIT listsChanged(listed());
        }
    });
    connect(&server, &slsk::ServerSession::watchUserReplyReceived, this,
            [this](const slsk::fromserver::WatchUserReply &reply) {
                presenceReceived(reply.username, reply.exists ? presenceOf(reply.status) : Presence::Offline);
            });
    connect(&server, &slsk::ServerSession::userStatusReceived, this,
            [this](const slsk::fromserver::UserStatusUpdate &update) {
                presenceReceived(update.username, presenceOf(update.status));
            });
}

QString SocialService::send(const QString &peer, const QString &text)
{
    const QString line = text.trimmed();
    if (peer.isEmpty() || line.isEmpty()) {
        return tr("Nothing to send.");
    }
    if (line.size() > kMaxTextLength) {
        return tr("That message is too long for Soulseek.");
    }
    if (!m_network.isOnline()) {
        return tr("Not connected to Soulseek.");
    }
    // Messages are paced and never repeated in a burst (SAFE-1).
    governor::Decision decision =
        m_governor.request(governor::Operation::PrivateMessage, {.recipient = peer, .text = line});
    if (!decision.permit) {
        return decision.explanation;
    }
    if (!m_network.sendPrivateMessage(peer, line, std::move(*decision.permit))) {
        return tr("The message couldn't be sent.");
    }
    const ChatMessage message{.id = 0, .peer = peer, .atMs = m_now(), .outgoing = true, .text = line, .read = true};
    if (const std::optional<qint64> id = store(message)) {
        ChatMessage stored = message;
        stored.id = *id;
        Q_EMIT messageAdded(stored);
    }
    publish();
    return {};
}

void SocialService::received(const slsk::fromserver::PrivateMessage &message)
{
    if (listOf(message.username) == UserList::Ignored) {
        m_activity.record(u"message"_s, message.username, u"ignored"_s, QString());
        return;
    }
    // Messages re-delivered after we were offline keep the server's time; new ones take ours, so a conversation stays
    // in order whatever the server's clock says.
    const qint64 at = !message.isNew && message.timestamp > 0 ? qint64{message.timestamp} * 1000 : m_now();
    const ChatMessage chat{
        .id = 0, .peer = message.username, .atMs = at, .outgoing = false, .text = message.message, .read = false};
    if (const std::optional<qint64> id = store(chat)) {
        ChatMessage stored = chat;
        stored.id = *id;
        Q_EMIT messageAdded(stored);
    }
    publish();
}

std::optional<qint64> SocialService::store(const ChatMessage &message)
{
    Statement insert =
        m_db.prepare(u"INSERT INTO messages (peer, at_ms, outgoing, text, read) VALUES (?1, ?2, ?3, ?4, ?5);");
    if (!insert.isValid() || !insert.bind(1, message.peer)
                                  .bind(2, message.atMs)
                                  .bind(3, message.outgoing ? 1 : 0)
                                  .bind(4, message.text)
                                  .bind(5, message.read ? 1 : 0)
                                  .run()) {
        qCWarning(lcSocial) << "couldn't keep a message with" << message.peer << ':' << m_db.lastError();
        return std::nullopt;
    }
    return m_db.lastInsertRowId();
}

QList<Conversation> SocialService::conversations()
{
    QList<Conversation> conversations;
    Statement query = m_db.prepare(
        u"SELECT m.peer, m.text, m.at_ms, m.outgoing, "
        "(SELECT count(*) FROM messages u WHERE u.peer = m.peer AND u.read = 0) "
        "FROM messages m WHERE m.id = (SELECT id FROM messages l WHERE l.peer = m.peer ORDER BY at_ms DESC, id DESC "
        "LIMIT 1) ORDER BY m.at_ms DESC;");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        conversations.append(Conversation{.peer = query.text(0),
                                          .lastText = query.text(1),
                                          .lastAtMs = query.int64(2),
                                          .lastOutgoing = query.int64(3) != 0,
                                          .unread = static_cast<int>(query.int64(4))});
    }
    return conversations;
}

QList<ChatMessage> SocialService::messages(const QString &peer, int limit)
{
    QList<ChatMessage> messages;
    Statement query = m_db.prepare(u"SELECT id, peer, at_ms, outgoing, text, read FROM messages WHERE peer = ?1 "
                                   "ORDER BY at_ms DESC, id DESC LIMIT ?2;");
    if (query.isValid()) {
        query.bind(1, peer).bind(2, std::max(1, limit));
        while (query.step() == Statement::Step::Row) {
            messages.prepend(readMessage(query));
        }
    }
    return messages;
}

void SocialService::markRead(const QString &peer)
{
    Statement update = m_db.prepare(u"UPDATE messages SET read = 1 WHERE peer = ?1 AND read = 0;");
    if (!update.isValid() || !update.bind(1, peer).run()) {
        qCWarning(lcSocial) << "couldn't mark" << peer << "read:" << m_db.lastError();
    }
    if (m_db.changes() > 0) {
        publish();
    }
}

void SocialService::deleteConversation(const QString &peer)
{
    Statement remove = m_db.prepare(u"DELETE FROM messages WHERE peer = ?1;");
    if (!remove.isValid() || !remove.bind(1, peer).run()) {
        qCWarning(lcSocial) << "couldn't delete the conversation with" << peer << ':' << m_db.lastError();
    }
    publish();
}

int SocialService::unread()
{
    Statement query = m_db.prepare(u"SELECT count(*) FROM messages WHERE read = 0;");
    return query.isValid() && query.step() == Statement::Step::Row ? static_cast<int>(query.int64(0)) : 0;
}

void SocialService::setListed(const QString &username, std::optional<UserList> list, const QString &note)
{
    const QString name = username.trimmed();
    if (name.isEmpty()) {
        return;
    }
    const std::optional<UserList> was = listOf(name);
    if (list) {
        Statement upsert = m_db.prepare(u"INSERT INTO user_lists (username, list, note, added_ms) VALUES (?1, ?2, ?3, "
                                        "?4) ON CONFLICT(username) DO UPDATE SET list = excluded.list, note = "
                                        "excluded.note;");
        if (!upsert.isValid() ||
            !upsert.bind(1, name).bind(2, userListName(*list)).bind(3, note).bind(4, m_now()).run()) {
            qCWarning(lcSocial) << "couldn't list" << name << ':' << m_db.lastError();
        }
    } else {
        Statement remove = m_db.prepare(u"DELETE FROM user_lists WHERE username = ?1;");
        if (!remove.isValid() || !remove.bind(1, name).run()) {
            qCWarning(lcSocial) << "couldn't unlist" << name << ':' << m_db.lastError();
        }
    }
    m_activity.record(u"user-list"_s, name, list ? userListName(*list) : u"unlisted"_s, QString());
    // Buddies are watched while we're online: the server then says when they come and go.
    if (was != UserList::Buddy && list == UserList::Buddy && m_network.isOnline() &&
        m_presence.size() < kMaxWatchedBuddies) {
        m_presence.insert(name, Presence::Unknown);
        m_network.server().watchUser(name);
    } else if (was == UserList::Buddy && list != UserList::Buddy && m_presence.remove(name) > 0) {
        m_network.server().unwatchUser(name);
    }
    Q_EMIT listsChanged(listed());
    if ((was == UserList::Banned) != (list == UserList::Banned)) {
        Q_EMIT bannedChanged(banned());
    }
}

QList<ListedUser> SocialService::listed()
{
    QList<ListedUser> users;
    Statement query =
        m_db.prepare(u"SELECT username, list, note, added_ms FROM user_lists ORDER BY list, username COLLATE NOCASE;");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        if (const std::optional<UserList> list = listFrom(query.text(1))) {
            const QString username = query.text(0);
            users.append(ListedUser{.username = username,
                                    .list = *list,
                                    .note = query.text(2),
                                    .addedMs = query.int64(3),
                                    .presence = m_presence.value(username, Presence::Unknown)});
        }
    }
    return users;
}

std::optional<UserList> SocialService::listOf(const QString &username)
{
    Statement query = m_db.prepare(u"SELECT list FROM user_lists WHERE username = ?1;");
    if (query.isValid() && query.bind(1, username).step() == Statement::Step::Row) {
        return listFrom(query.text(0));
    }
    return std::nullopt;
}

QSet<QString> SocialService::banned()
{
    QSet<QString> users;
    Statement query = m_db.prepare(u"SELECT username FROM user_lists WHERE list = 'banned';");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        users.insert(query.text(0));
    }
    return users;
}

void SocialService::publish()
{
    Q_EMIT conversationsChanged(conversations());
}

void SocialService::watchBuddies()
{
    m_presence.clear();
    Statement query = m_db.prepare(u"SELECT username FROM user_lists WHERE list = 'buddy' ORDER BY added_ms LIMIT ?1;");
    if (query.isValid()) {
        query.bind(1, kMaxWatchedBuddies);
        while (query.step() == Statement::Step::Row) {
            const QString username = query.text(0);
            m_presence.insert(username, Presence::Unknown);
            m_network.server().watchUser(username);
        }
    }
}

void SocialService::presenceReceived(const QString &username, Presence presence)
{
    const auto known = m_presence.find(username);
    if (known == m_presence.end() || *known == presence) {
        return; // not a buddy we watch, or nothing new
    }
    *known = presence;
    Q_EMIT listsChanged(listed());
}

} // namespace selecta::core
