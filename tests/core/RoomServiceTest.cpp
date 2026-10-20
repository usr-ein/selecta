// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RoomService.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/NetworkService.h"
#include "core/SocialService.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestUtil.h"
#include "governor/Governor.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::Room;
using selecta::core::RoomService;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;
namespace toserver = selecta::slsk::toserver;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

class RoomServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        network = std::make_unique<selecta::core::NetworkService>(governor, log, tokens, [] { return kNow; });
        social = std::make_unique<selecta::core::SocialService>(db, governor, *network, log, [] { return kNow; });
        rooms = std::make_unique<RoomService>(db, governor, *network, *social, log, [] { return kNow; });
    }

    void TearDown() override
    {
        rooms.reset();
        social.reset();
        network.reset();
    }

    void goOnline()
    {
        selecta::core::NetworkConfig config;
        config.host = server.address().toString();
        config.port = server.port();
        config.username = u"selecta_test"_s;
        config.listenPort = 0;
        network->start(config);
        network->setPassword(u"pw"_s);
        ASSERT_TRUE(waitFor([this] { return network->isOnline(); }));
    }

    std::optional<Room> room(const QString &name) const
    {
        const QList<Room> all = rooms->rooms();
        for (const Room &candidate : all) {
            if (candidate.name == name) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    bool joined(const QString &name) const
    {
        const std::optional<Room> found = room(name);
        return found && found->state == Room::State::Joined;
    }

    FakeServer server;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    std::unique_ptr<selecta::core::NetworkService> network;
    std::unique_ptr<selecta::core::SocialService> social;
    std::unique_ptr<RoomService> rooms;
};

} // namespace

TEST_F(RoomServiceTest, JoinsReadsAndPostsInARoom)
{
    goOnline();
    EXPECT_EQ(rooms->join(u"dnb"_s), QString());
    ASSERT_TRUE(waitFor([this] { return joined(u"dnb"_s); }));
    EXPECT_EQ(room(u"dnb"_s)->members, QStringList{u"selecta_test"_s});

    // Someone comes in and names us: the line is unread, and a mention.
    server.joinRoomAs(u"lossless_lena"_s, u"dnb"_s);
    ASSERT_TRUE(waitFor([this] { return room(u"dnb"_s)->members.size() == 2; }));
    ASSERT_TRUE(server.sayInRoom(u"lossless_lena"_s, u"dnb"_s, u"selecta_test: got the B-side?"_s));
    ASSERT_TRUE(waitFor([this] { return room(u"dnb"_s)->unread == 1; }));
    EXPECT_EQ(room(u"dnb"_s)->mentions, 1);
    ASSERT_EQ(rooms->lines(u"dnb"_s).size(), 1);
    EXPECT_EQ(rooms->lines(u"dnb"_s).constFirst().user, u"lossless_lena"_s);
    EXPECT_TRUE(rooms->lines(u"dnb"_s).constFirst().mention);

    // Looking at the room reads it; lines that come while it's on screen stay read.
    rooms->setViewing(u"dnb"_s);
    ASSERT_TRUE(waitFor([this] { return room(u"dnb"_s)->unread == 0; }));
    ASSERT_TRUE(server.sayInRoom(u"lossless_lena"_s, u"dnb"_s, u"selecta_testing is not you"_s));
    ASSERT_TRUE(waitFor([this] { return rooms->lines(u"dnb"_s).size() == 2; }));
    EXPECT_FALSE(rooms->lines(u"dnb"_s).at(1).mention); // not the name as a word of its own
    QTest::qWait(400);
    EXPECT_EQ(room(u"dnb"_s)->unread, 0);

    // Our own line shows when the server echoes it; the next one waits for the governor.
    EXPECT_EQ(rooms->say(u"dnb"_s, u"yes, 24/96"_s), QString());
    ASSERT_TRUE(waitFor([this] { return rooms->lines(u"dnb"_s).size() == 3; }));
    EXPECT_TRUE(rooms->lines(u"dnb"_s).at(2).own);
    EXPECT_NE(rooms->say(u"dnb"_s, u"and the A-side"_s), QString());
    EXPECT_NE(rooms->say(u"elsewhere"_s, u"hello"_s), QString()); // not in it

    const QSignalSpy closed(rooms.get(), &RoomService::roomClosed);
    rooms->leave(u"dnb"_s);
    EXPECT_EQ(closed.size(), 1);
    EXPECT_FALSE(room(u"dnb"_s).has_value());
    ASSERT_TRUE(waitFor([this] { return server.receivedOf<toserver::LeaveRoom>(u"selecta_test"_s).size() == 1; }));
}

TEST_F(RoomServiceTest, JoinsArePacedCappedAndChecked)
{
    EXPECT_EQ(rooms->join(u"dnb"_s), u"Not connected to Soulseek."_s);
    goOnline();
    EXPECT_NE(rooms->join(u" spaced"_s), QString());
    EXPECT_NE(rooms->join(QString()), QString());
    EXPECT_NE(rooms->join(u"a name far longer than twenty-four"_s), QString());

    // One join every 2 s.
    const auto joinedAt = std::make_shared<QList<qint64>>(); // shared with the connection, which outlives the test
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&server, &FakeServer::messageReceived, &server,
                     [joinedAt, elapsed](const QString &, const selecta::slsk::ToServerMessage &message) {
                         if (std::holds_alternative<toserver::JoinRoom>(message)) {
                             joinedAt->append(elapsed.elapsed());
                         }
                     });
    for (const QString &name : {u"one"_s, u"two"_s, u"three"_s}) {
        EXPECT_EQ(rooms->join(name), QString());
    }
    ASSERT_TRUE(waitFor([&] { return joinedAt->size() == 3; }, 10s));
    // No two joins closer than 2 s (less a little for timer rounding).
    const QList<qint64> &times = *joinedAt;
    EXPECT_EQ(std::ranges::adjacent_find(times, [](qint64 a, qint64 b) { return b - a < 1900; }), times.end());
    ASSERT_TRUE(waitFor([this] { return joined(u"three"_s); }));

    // Ten rooms at most.
    for (int i = 4; i <= RoomService::kMaxRooms; ++i) {
        EXPECT_EQ(rooms->join(u"room %1"_s.arg(i)), QString());
    }
    EXPECT_NE(rooms->join(u"eleventh"_s), QString());
    EXPECT_EQ(rooms->rooms().size(), RoomService::kMaxRooms);
}

TEST_F(RoomServiceTest, TheListComesAtMostOnceAMinute)
{
    FakeServer::Welcome welcome;
    welcome.rooms = {{u"dnb"_s, 40}, {u"nicotine"_s, 120}};
    server.setWelcome(welcome);
    const QSignalSpy listed(rooms.get(), &RoomService::listChanged);
    goOnline();
    // The server sends a list after the login: the biggest rooms first.
    ASSERT_TRUE(waitFor([&] { return !listed.isEmpty(); }));
    ASSERT_EQ(rooms->list().size(), 2);
    EXPECT_EQ(rooms->list().constFirst().name, u"nicotine"_s);
    EXPECT_EQ(rooms->list().constFirst().users, 120);

    rooms->refreshList();
    ASSERT_TRUE(waitFor([this] { return server.receivedOf<toserver::RoomList>(u"selecta_test"_s).size() == 1; }));
    rooms->refreshList(); // too soon: the list we have answers
    QTest::qWait(200);
    EXPECT_EQ(server.receivedOf<toserver::RoomList>(u"selecta_test"_s).size(), 1);
}

TEST_F(RoomServiceTest, RoomsOpenAtQuitAreJoinedAgainAndIgnoredUsersArentHeard)
{
    goOnline();
    social->setListed(u"spammer"_s, selecta::core::UserList::Ignored);
    ASSERT_EQ(rooms->join(u"dnb"_s), QString());
    ASSERT_TRUE(waitFor([this] { return joined(u"dnb"_s); }));
    server.joinRoomAs(u"spammer"_s, u"dnb"_s);
    ASSERT_TRUE(server.sayInRoom(u"spammer"_s, u"dnb"_s, u"buy followers"_s));
    QTest::qWait(200);
    EXPECT_TRUE(rooms->lines(u"dnb"_s).isEmpty());

    // Quit while in the room; the next session joins it again once logged in.
    rooms.reset();
    network->disconnectNow();
    ASSERT_TRUE(waitFor([this] { return !network->isOnline(); }));
    rooms = std::make_unique<RoomService>(db, governor, *network, *social, log, [] { return kNow; });
    ASSERT_TRUE(room(u"dnb"_s).has_value());
    EXPECT_EQ(room(u"dnb"_s)->state, Room::State::Waiting);
    clock.advance(10min); // the governor's reconnect pacing
    network->connectNow();
    ASSERT_TRUE(waitFor([this] { return joined(u"dnb"_s); }, 15s));

    // Left, it's forgotten.
    rooms->leave(u"dnb"_s);
    rooms = std::make_unique<RoomService>(db, governor, *network, *social, log, [] { return kNow; });
    EXPECT_TRUE(rooms->rooms().isEmpty());
}
