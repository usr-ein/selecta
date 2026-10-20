// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/ServerMessages.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}

namespace selecta::core {

class ActivityLog;
class NetworkService;

// One private message (PRD NET-7).
struct ChatMessage
{
    qint64 id = 0;
    QString peer;
    qint64 atMs = 0; // Unix time, ms
    bool outgoing = false;
    QString text;
    bool read = false;

    friend bool operator==(const ChatMessage &, const ChatMessage &) = default;
};

// A conversation with one user, as the list shows it.
struct Conversation
{
    QString peer;
    QString lastText;
    qint64 lastAtMs = 0;
    bool lastOutgoing = false;
    int unread = 0;

    friend bool operator==(const Conversation &, const Conversation &) = default;
};

// The user lists (PRD NET-8): buddies; ignored users, whose messages are dropped; banned users, who get "Banned"
// for any upload and nothing of our shares.
enum class UserList { Buddy, Ignored, Banned };

// Whether a buddy is on Soulseek, as the server says while we watch them; Unknown while we're offline.
enum class Presence { Unknown, Offline, Away, Online };

struct ListedUser
{
    QString username;
    UserList list = UserList::Buddy;
    QString note;
    qint64 addedMs = 0;
    Presence presence = Presence::Unknown; // buddies only

    friend bool operator==(const ListedUser &, const ListedUser &) = default;
};

QString userListName(UserList list);

// Private messages and the user lists (PRD NET-7, NET-8) on the engine thread. Messages are kept in the database;
// sending goes through the governor (spacing, no repeats). Messages from ignored users are dropped, unread. Buddies
// are watched while we're online, so the list says who is on Soulseek.
//
// Threading: engine thread. Ownership: every collaborator outlives it.
class SocialService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;
    static constexpr int kMaxTextLength = 2000;    // characters: the server's line limit is in bytes, checked by slsk
    static constexpr int kMaxWatchedBuddies = 200; // the server is told about this many buddies at most

    SocialService(storage::Database &db, governor::Governor &governor, NetworkService &network, ActivityLog &activity,
                  WallClock now = {}, QObject *parent = nullptr);

    // Sends a message; an empty string when it went, else why it couldn't (offline, the governor, too long).
    QString send(const QString &peer, const QString &text);
    QList<Conversation> conversations();
    // A conversation's messages, oldest first (the latest `limit`).
    QList<ChatMessage> messages(const QString &peer, int limit = 500);
    void markRead(const QString &peer);
    void deleteConversation(const QString &peer);
    int unread();

    // Puts a user on a list, or takes them off every list (nullopt).
    void setListed(const QString &username, std::optional<UserList> list, const QString &note = {});
    QList<ListedUser> listed();
    std::optional<UserList> listOf(const QString &username);
    QSet<QString> banned();

    // --- fed by the network ---
    void received(const slsk::fromserver::PrivateMessage &message);

Q_SIGNALS:
    void conversationsChanged(const QList<selecta::core::Conversation> &conversations);
    void messageAdded(const selecta::core::ChatMessage &message);
    void listsChanged(const QList<selecta::core::ListedUser> &users);
    // The banned users changed (ShareService refuses them).
    void bannedChanged(const QSet<QString> &banned);

private:
    std::optional<qint64> store(const ChatMessage &message);
    void publish();
    void watchBuddies();
    void presenceReceived(const QString &username, Presence presence);

    QHash<QString, Presence> m_presence; // the buddies we watch

    storage::Database &m_db;
    governor::Governor &m_governor;
    NetworkService &m_network;
    ActivityLog &m_activity;
    WallClock m_now;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ChatMessage)
Q_DECLARE_METATYPE(selecta::core::Conversation)
Q_DECLARE_METATYPE(selecta::core::ListedUser)
