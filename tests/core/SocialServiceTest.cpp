// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SocialService.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/NetworkService.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestUtil.h"
#include "governor/Governor.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::Presence;
using selecta::core::SocialService;
using selecta::core::UserList;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;
namespace fromserver = selecta::slsk::fromserver;
namespace toserver = selecta::slsk::toserver;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

class SocialServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        network = std::make_unique<selecta::core::NetworkService>(governor, log, tokens, [this] { return now; });
        social = std::make_unique<SocialService>(db, governor, *network, log, [this] { return now; });
    }

    void TearDown() override
    {
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

    QList<toserver::MessageUser> sent() const { return server.receivedOf<toserver::MessageUser>(u"selecta_test"_s); }

    // How often the server was asked to watch this user (the session watches our own name too).
    qsizetype watched(const QString &username) const
    {
        const QList<toserver::WatchUser> watches = server.receivedOf<toserver::WatchUser>(u"selecta_test"_s);
        return std::ranges::count_if(watches,
                                     [&](const toserver::WatchUser &watch) { return watch.username == username; });
    }

    std::optional<selecta::core::ListedUser> listed(const QString &username) const
    {
        const QList<selecta::core::ListedUser> users = social->listed();
        for (const selecta::core::ListedUser &user : users) {
            if (user.username == username) {
                return user;
            }
        }
        return std::nullopt;
    }

    FakeServer server;
    qint64 now = kNow;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    std::unique_ptr<selecta::core::NetworkService> network;
    std::unique_ptr<SocialService> social;
};

} // namespace

TEST_F(SocialServiceTest, SendsAndReceivesMessagesAndKeepsThem)
{
    goOnline();
    EXPECT_EQ(social->send(u"lossless_lena"_s, u"  Is the Night Signals rip from vinyl?  "_s), QString());
    ASSERT_TRUE(waitFor([this] { return sent().size() == 1; }));
    EXPECT_EQ(sent().constFirst().username, u"lossless_lena"_s);
    EXPECT_EQ(sent().constFirst().message, u"Is the Night Signals rip from vinyl?"_s);

    const QSignalSpy added(social.get(), &SocialService::messageAdded);
    now += 60'000;
    ASSERT_TRUE(server.sendPrivateMessage(u"lossless_lena"_s, u"selecta_test"_s, u"Yes, 24/96 from the LP"_s));
    ASSERT_TRUE(waitFor([&] { return added.size() == 1; }));
    const QList<selecta::core::Conversation> conversations = social->conversations();
    ASSERT_EQ(conversations.size(), 1);
    EXPECT_EQ(conversations.constFirst().peer, u"lossless_lena"_s);
    EXPECT_EQ(conversations.constFirst().lastText, u"Yes, 24/96 from the LP"_s);
    EXPECT_FALSE(conversations.constFirst().lastOutgoing);
    EXPECT_EQ(conversations.constFirst().unread, 1);
    EXPECT_EQ(social->unread(), 1);

    // Oldest first, ours marked as ours and read.
    const QList<selecta::core::ChatMessage> messages = social->messages(u"lossless_lena"_s);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_TRUE(messages.at(0).outgoing);
    EXPECT_TRUE(messages.at(0).read);
    EXPECT_FALSE(messages.at(1).outgoing);
    EXPECT_FALSE(messages.at(1).read);

    social->markRead(u"lossless_lena"_s);
    EXPECT_EQ(social->unread(), 0);

    // The server's own messages are notices, not a conversation.
    ASSERT_TRUE(server.sendPrivateMessage(u"server"_s, u"selecta_test"_s, u"Scheduled maintenance tonight"_s));
    QTest::qWait(200);
    EXPECT_EQ(social->conversations().size(), 1);

    social->deleteConversation(u"lossless_lena"_s);
    EXPECT_TRUE(social->conversations().isEmpty());
    EXPECT_TRUE(social->messages(u"lossless_lena"_s).isEmpty());
}

TEST_F(SocialServiceTest, MessagesArePacedAndNeverRepeatedOrSentOffline)
{
    EXPECT_EQ(social->send(u"lossless_lena"_s, u"hello"_s), u"Not connected to Soulseek."_s);
    goOnline();
    EXPECT_EQ(social->send(u"lossless_lena"_s, u"hello"_s), QString());
    // Too soon after the last one: the governor says when the next may go.
    EXPECT_NE(social->send(u"lossless_lena"_s, u"are you there?"_s), QString());
    clock.advance(5s);
    // The same words to the same user again within minutes are refused.
    EXPECT_NE(social->send(u"lossless_lena"_s, u"hello"_s), QString());
    EXPECT_EQ(social->send(u"lossless_lena"_s, u"are you there?"_s), QString());
    clock.advance(5s);
    EXPECT_NE(social->send(u"lossless_lena"_s, QString(SocialService::kMaxTextLength + 1, u'a')), QString());
    EXPECT_NE(social->send(u"lossless_lena"_s, u"   "_s), QString());
    ASSERT_TRUE(waitFor([this] { return sent().size() == 2; }));
    QTest::qWait(100);
    EXPECT_EQ(sent().size(), 2);
    EXPECT_EQ(social->messages(u"lossless_lena"_s).size(), 2);
}

TEST_F(SocialServiceTest, IgnoredUsersAreDroppedAndBansAreAnnounced)
{
    goOnline();
    const QSignalSpy banned(social.get(), &SocialService::bannedChanged);
    social->setListed(u"spammer"_s, UserList::Ignored);
    social->setListed(u"leech"_s, UserList::Banned);
    ASSERT_EQ(banned.size(), 1);
    EXPECT_EQ(social->banned(), QSet<QString>{u"leech"_s});

    const QSignalSpy added(social.get(), &SocialService::messageAdded);
    ASSERT_TRUE(server.sendPrivateMessage(u"spammer"_s, u"selecta_test"_s, u"buy followers"_s));
    ASSERT_TRUE(server.sendPrivateMessage(u"lossless_lena"_s, u"selecta_test"_s, u"hi"_s));
    ASSERT_TRUE(waitFor([&] { return added.size() == 1; }));
    QTest::qWait(100);
    const QList<selecta::core::Conversation> conversations = social->conversations();
    ASSERT_EQ(conversations.size(), 1);
    EXPECT_EQ(conversations.constFirst().peer, u"lossless_lena"_s);

    // Moving between lists and off them; the ban list is announced only when it changes.
    social->setListed(u"spammer"_s, UserList::Buddy);
    EXPECT_EQ(banned.size(), 1);
    social->setListed(u"leech"_s, std::nullopt);
    ASSERT_EQ(banned.size(), 2);
    EXPECT_TRUE(social->banned().isEmpty());
    ASSERT_TRUE(listed(u"spammer"_s).has_value());
    EXPECT_EQ(listed(u"spammer"_s)->list, UserList::Buddy);
    EXPECT_FALSE(listed(u"leech"_s).has_value());
    EXPECT_EQ(social->listOf(u"spammer"_s), UserList::Buddy);
}

TEST_F(SocialServiceTest, BuddiesAreWatchedWhileOnline)
{
    social->setListed(u"kassia"_s, UserList::Buddy); // offline: nothing to tell the server yet
    EXPECT_EQ(listed(u"kassia"_s)->presence, Presence::Unknown);
    goOnline();
    ASSERT_TRUE(waitFor([this] { return watched(u"kassia"_s) == 1; }));
    // The server doesn't know her: offline.
    ASSERT_TRUE(waitFor([this] { return listed(u"kassia"_s)->presence == Presence::Offline; }));

    // She comes online.
    ASSERT_TRUE(server.sendTo(u"selecta_test"_s,
                              fromserver::UserStatusUpdate{u"kassia"_s, selecta::slsk::UserStatus::Online, false}));
    ASSERT_TRUE(waitFor([this] { return listed(u"kassia"_s)->presence == Presence::Online; }));
    // Someone who isn't a buddy changes nothing.
    ASSERT_TRUE(server.sendTo(u"selecta_test"_s,
                              fromserver::UserStatusUpdate{u"stranger"_s, selecta::slsk::UserStatus::Away, false}));
    QTest::qWait(100);
    EXPECT_FALSE(listed(u"stranger"_s).has_value());

    // A new buddy is watched at once; one taken off the list isn't any more.
    social->setListed(u"vinyl_ghost"_s, UserList::Buddy);
    ASSERT_TRUE(waitFor([this] { return watched(u"vinyl_ghost"_s) == 1; }));
    ASSERT_TRUE(waitFor([this] { return listed(u"vinyl_ghost"_s)->presence == Presence::Offline; }));
    social->setListed(u"kassia"_s, std::nullopt);
    ASSERT_TRUE(waitFor([this] { return server.receivedOf<toserver::UnwatchUser>(u"selecta_test"_s).size() == 1; }));

    // Offline again: nothing is known.
    network->disconnectNow();
    ASSERT_TRUE(waitFor([this] { return listed(u"vinyl_ghost"_s)->presence == Presence::Unknown; }));
}
