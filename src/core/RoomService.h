// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/ServerMessages.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <chrono>
#include <functional>

class QTimer;

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}

namespace selecta::core {

class ActivityLog;
class NetworkService;
class SocialService;

// A room in the server's list (PRD NET-10).
struct RoomListing
{
    QString name;
    int users = 0;

    friend bool operator==(const RoomListing &, const RoomListing &) = default;
};

// A line said in a room. The server keeps no history: a room shows what was said while we were in it.
struct RoomLine
{
    qint64 atMs = 0; // Unix time, ms
    QString user;
    QString text;
    bool own = false;     // our own line, as the server echoed it back
    bool mention = false; // it names us

    friend bool operator==(const RoomLine &, const RoomLine &) = default;
};

// A room we are in, or on our way into.
struct Room
{
    enum class State {
        Waiting, // to be joined when we're online and the last join is far enough
        Joining,
        Joined,
        Failed, // the server refused it (a private room): `message` says so
    };

    QString name;
    State state = State::Waiting;
    QStringList members; // sorted, case-insensitively
    int unread = 0;      // lines that arrived while it wasn't on screen
    int mentions = 0;    // of those, the ones that name us
    QString message;

    friend bool operator==(const Room &, const Room &) = default;
};

// Minimal chat rooms (PRD NET-10, D32) on the engine thread: the room list, joining and leaving, reading and posting;
// no tickers, private rooms or room administration. Everything it sends is a user action, paced as the protocol notes
// ask (references/soulseek-protocol.md §12.7): the room list at most once a minute, one join every 2 s and 10 rooms at
// most; lines go through the governor, in the private messages' budget. Rooms open at quit are joined again at the
// next login. Lines from ignored users (NET-8) are dropped.
//
// Threading: engine thread. Ownership: every collaborator outlives it.
class RoomService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;
    static constexpr int kMaxRooms = 10;
    static constexpr std::chrono::milliseconds kJoinSpacing{2000};
    static constexpr std::chrono::milliseconds kListInterval{60'000};
    static constexpr int kKeptLines = 500;           // per room, in memory: the server keeps nothing either
    static constexpr qsizetype kShownLineMax = 4096; // characters of a line received

    RoomService(storage::Database &db, governor::Governor &governor, NetworkService &network, SocialService &social,
                ActivityLog &activity, WallClock now = {}, QObject *parent = nullptr);

    // Asks the server for every room, at most once a minute; listChanged() follows.
    void refreshList();
    QList<RoomListing> list() const { return m_list; }
    // Joins a room (a name nobody uses creates it). Returns why it can't, or an empty string.
    QString join(const QString &room);
    void leave(const QString &room);
    // Posts a line; returns why it can't go, or an empty string. It shows when the server echoes it back.
    QString say(const QString &room, const QString &text);
    // The room on screen (empty: none): its lines count as read.
    void setViewing(const QString &room);
    QList<Room> rooms() const;
    QList<RoomLine> lines(const QString &room) const { return m_lines.value(room); }

Q_SIGNALS:
    void listChanged(const QList<selecta::core::RoomListing> &rooms);
    void roomChanged(const selecta::core::Room &room);
    void roomClosed(const QString &room);
    void lineAdded(const QString &room, const selecta::core::RoomLine &line);

private:
    void loggedIn();
    void wentOffline();
    void pumpJoins();
    void listReceived(const slsk::fromserver::RoomList &list);
    void joined(const slsk::fromserver::RoomJoined &room);
    void joinFailed(const QString &room);
    void left(const QString &room);
    void userJoined(const slsk::fromserver::RoomUserJoined &event);
    void userLeft(const slsk::fromserver::RoomUserLeft &event);
    void lineReceived(const slsk::fromserver::RoomMessage &message);
    // Room changes are many in a busy room: they go out together, a moment later.
    void changed(const QString &room);
    void flush();
    void remember(const QString &room, bool open);

    storage::Database &m_db;
    governor::Governor &m_governor;
    NetworkService &m_network;
    SocialService &m_social;
    ActivityLog &m_activity;
    WallClock m_now;
    QList<RoomListing> m_list;
    QHash<QString, Room> m_rooms;
    QHash<QString, QList<RoomLine>> m_lines;
    QStringList m_joinQueue;
    QSet<QString> m_dirty;
    QString m_viewing;
    QElapsedTimer m_lastJoin;
    QElapsedTimer m_lastList;
    QTimer *m_joinTimer = nullptr;
    QTimer *m_flushTimer = nullptr;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::RoomListing)
Q_DECLARE_METATYPE(selecta::core::RoomLine)
Q_DECLARE_METATYPE(selecta::core::Room)
