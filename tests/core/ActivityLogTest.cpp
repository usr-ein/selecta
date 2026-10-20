// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ActivityLog.h"

#include "base/Clock.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>

#include <array>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::ActivityEntry;
using selecta::core::ActivityLog;

namespace {

constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

class ActivityLogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    int count()
    {
        auto query = db.prepare(u"SELECT COUNT(*) FROM activity;");
        return query.step() == selecta::storage::Statement::Step::Row ? static_cast<int>(query.int64(0)) : -1;
    }

    selecta::storage::Database db;
    qint64 nowMs = 1'800'000'000'000;
    ActivityLog log{db, [this] { return nowMs; }};
};

} // namespace

TEST_F(ActivityLogTest, RecordsAndListsNewestFirst)
{
    const auto first = log.record(u"search"_s, u"neon rain"_s, u"allowed"_s, u"Search sent."_s);
    nowMs += 1000;
    const auto second = log.record(u"server"_s, QString(), u"connected"_s, u"Logged in."_s);
    ASSERT_GT(first.id, 0);
    ASSERT_GT(second.id, first.id);

    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].id, second.id);
    EXPECT_EQ(entries[0].operation, u"server"_s);
    EXPECT_EQ(entries[1].subject, u"neon rain"_s);
    EXPECT_EQ(entries[1].atMs, nowMs - 1000);
    EXPECT_EQ(ActivityLog::recent(db, 1).size(), 1);
    EXPECT_TRUE(ActivityLog::recent(db, -5).isEmpty());
}

TEST_F(ActivityLogTest, EmitsEachLineWithItsId)
{
    const QSignalSpy spy(&log, &ActivityLog::recorded);
    const auto entry = log.record(u"browse"_s, u"vinyl_ghost"_s, u"allowed"_s, QString());
    ASSERT_EQ(spy.size(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ActivityEntry>().id, entry.id);
}

TEST_F(ActivityLogTest, RecordsGovernorDecisions)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    QObject::connect(&governor, &selecta::governor::Governor::decided, &log, &ActivityLog::recordDecision);

    governor.request(selecta::governor::Operation::Search, {.query = u"okuma drift"_s});
    governor.setPaused(true);
    governor.request(selecta::governor::Operation::Browse, {.peer = u"vinyl_ghost"_s});

    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[1].operation, u"search"_s);
    EXPECT_EQ(entries[1].outcome, u"allowed"_s);
    EXPECT_EQ(entries[0].operation, u"browse"_s);
    EXPECT_EQ(entries[0].subject, u"vinyl_ghost"_s);
    EXPECT_EQ(entries[0].outcome, u"network-paused"_s);
    EXPECT_FALSE(entries[0].explanation.isEmpty());
}

TEST_F(ActivityLogTest, PrivateMessageTextIsNeverLogged)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    QObject::connect(&governor, &selecta::governor::Governor::decided, &log, &ActivityLog::recordDecision);
    governor.request(selecta::governor::Operation::PrivateMessage,
                     {.recipient = u"friend"_s, .text = u"my secret plans"_s});
    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].subject, u"friend"_s);
    EXPECT_FALSE(entries[0].explanation.contains(u"secret"_s));
}

TEST_F(ActivityLogTest, PrunesByAge)
{
    log.record(u"search"_s, u"old"_s, u"allowed"_s, QString());
    nowMs += (ActivityLog::kKeepDays + 1) * kDayMs;
    log.record(u"search"_s, u"new"_s, u"allowed"_s, QString());
    EXPECT_EQ(log.prune(), 1);
    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].subject, u"new"_s);
}

TEST_F(ActivityLogTest, PrunesToTheNewestRows)
{
    // Bulk-insert past the cap in one statement; the newest rows (highest ids) must be the ones kept.
    auto fill = db.prepare(u"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n WHERE x < ?1) "
                           u"INSERT INTO activity (at, operation, subject, outcome) "
                           u"SELECT ?2, 'search', CAST(x AS TEXT), 'allowed' FROM n;");
    ASSERT_TRUE(fill.bind(1, ActivityLog::kMaxRows + 25).bind(2, nowMs).run()) << db.lastError().toStdString();
    EXPECT_EQ(log.prune(), 25);
    EXPECT_EQ(count(), ActivityLog::kMaxRows);
    EXPECT_EQ(ActivityLog::recent(db, 1).constFirst().subject, QString::number(ActivityLog::kMaxRows + 25));
    EXPECT_EQ(log.prune(), 0);
}

TEST_F(ActivityLogTest, AFailedWriteIsReportedNotThrown)
{
    ASSERT_TRUE(db.exec(u"DROP TABLE activity;"));
    const QSignalSpy spy(&log, &ActivityLog::recorded);
    EXPECT_EQ(log.record(u"search"_s, u"x"_s, u"allowed"_s, QString()).id, 0);
    EXPECT_EQ(spy.size(), 0);
    EXPECT_EQ(log.prune(), -1);
}

// --- who started it and why (P-006) ---------------------------------------------------------------------------------

namespace {

using selecta::governor::Operation;
using selecta::governor::Refusal;
using selecta::slsk::ConnectPurpose;

ActivityEntry line(const QString &operation, const QString &subject, const QString &outcome,
                   const QString &explanation = {})
{
    return ActivityEntry{
        .id = 1, .atMs = 0, .operation = operation, .subject = subject, .outcome = outcome, .explanation = explanation};
}

QString describe(const QString &operation, const QString &subject, const QString &outcome,
                 const QString &explanation = {})
{
    return ActivityLog::describe(line(operation, subject, outcome, explanation));
}

} // namespace

TEST_F(ActivityLogTest, ALookupSaysWhyForEachPurpose)
{
    log.recordPeerLookup(u"searcher"_s, ConnectPurpose::SearchResponse, Refusal::None, u"Peer lookups: allowed."_s);
    log.recordPeerLookup(u"uploader"_s, ConnectPurpose::Transfer, Refusal::None, u"Peer lookups: allowed."_s);
    log.recordPeerLookup(u"parent"_s, ConnectPurpose::Distributed, Refusal::None, u"Peer lookups: allowed."_s);
    log.recordPeerLookup(u"someone"_s, ConnectPurpose::Request, Refusal::None, u"Peer lookups: allowed."_s);

    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 4);
    for (const ActivityEntry &entry : entries) {
        EXPECT_EQ(entry.operation, u"peer-lookup"_s);
        EXPECT_EQ(entry.outcome, u"allowed"_s);
        EXPECT_EQ(ActivityLog::describe(entry), entry.explanation); // stored as it's shown
    }
    EXPECT_EQ(entries[3].subject, u"searcher"_s);
    EXPECT_EQ(entries[3].explanation, u"You looked up searcher's address to send them your search results: they "
                                      u"searched for something you share."_s);
    EXPECT_EQ(entries[2].explanation, u"You looked up uploader's address to carry on a file transfer between you (your "
                                      u"download from them, or theirs from you)."_s);
    EXPECT_EQ(entries[1].explanation, u"You looked up parent's address to try them as your parent in the distributed "
                                      u"network, which passes other users' searches on to you."_s);
    // Nothing asked of them just before: the request isn't named.
    EXPECT_EQ(entries[0].explanation, u"You looked up someone's address to send them one of your requests."_s);
}

TEST_F(ActivityLogTest, ALookupForOurRequestNamesTheRequest)
{
    struct Case
    {
        Operation asked;
        QString reason;
    };
    const std::array cases{
        Case{Operation::Browse, u"browse their shares"_s},
        Case{Operation::UserInfo, u"ask for their user info"_s},
        Case{Operation::FolderContents, u"ask what's in one of their folders"_s},
        Case{Operation::QueueDownload, u"ask them to queue a download for you"_s},
        Case{Operation::PlaceInQueue, u"ask where your download stands in their queue"_s},
    };
    for (const auto &c : cases) {
        log.recordDecision(c.asked, u"vinyl_ghost"_s, Refusal::None, u"Allowed."_s);
        nowMs += 100;
        log.recordPeerLookup(u"vinyl_ghost"_s, ConnectPurpose::Request, Refusal::None, QString());
        const ActivityEntry lookup = ActivityLog::recent(db, 1).constFirst();
        EXPECT_EQ(lookup.explanation, u"You looked up vinyl_ghost's address to %1."_s.arg(c.reason));
    }
}

TEST_F(ActivityLogTest, OnlyARecentAllowedRequestOfThatPeerNamesALookup)
{
    // Another peer's request says nothing about this one.
    log.recordDecision(Operation::Browse, u"other"_s, Refusal::None, u"Allowed."_s);
    log.recordPeerLookup(u"vinyl_ghost"_s, ConnectPurpose::Request, Refusal::None, QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"one of your requests."_s));

    // A refused request sent nothing, so no lookup follows from it.
    log.recordDecision(Operation::Browse, u"vinyl_ghost"_s, Refusal::RateLimited, u"Browsing: wait."_s);
    log.recordPeerLookup(u"vinyl_ghost"_s, ConnectPurpose::Request, Refusal::None, QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"one of your requests."_s));

    // Too old to be what this lookup is for.
    log.recordDecision(Operation::QueueDownload, u"vinyl_ghost"_s, Refusal::None, u"Download allowed."_s);
    nowMs += ActivityLog::kRequestMemoryMs + 1;
    log.recordPeerLookup(u"vinyl_ghost"_s, ConnectPurpose::Request, Refusal::None, QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"one of your requests."_s));

    // A lookup for another purpose isn't named after the request.
    log.recordDecision(Operation::QueueDownload, u"vinyl_ghost"_s, Refusal::None, u"Download allowed."_s);
    log.recordPeerLookup(u"vinyl_ghost"_s, ConnectPurpose::SearchResponse, Refusal::None, QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.contains(u"search results"_s));
}

TEST_F(ActivityLogTest, ARefusedLookupSaysWhyItWasHeldBack)
{
    log.recordPeerLookup(u"searcher"_s, ConnectPurpose::SearchResponse, Refusal::RateLimited,
                         u"Peer lookups: too many at once, retry in 4 s."_s);
    log.recordPeerLookup(u"parent"_s, ConnectPurpose::Distributed, Refusal::NetworkPaused, QString());
    const auto entries = ActivityLog::recent(db, 2);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[1].outcome, u"rate-limited"_s);
    EXPECT_EQ(entries[1].explanation,
              u"You wanted to look up searcher's address to send them your search results: they searched for "
              u"something you share. Selecta held it back: Peer lookups: too many at once, retry in 4 s."_s);
    EXPECT_EQ(entries[0].outcome, u"network-paused"_s);
    EXPECT_TRUE(entries[0].explanation.startsWith(u"You wanted to look up parent's address to try them"_s));
    EXPECT_TRUE(entries[0].explanation.endsWith(u"Selecta held it back."_s));
}

TEST_F(ActivityLogTest, GovernorLookupDecisionsAreLeftToRecordPeerLookup)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    QObject::connect(&governor, &selecta::governor::Governor::decided, &log, &ActivityLog::recordDecision);
    governor.request(Operation::PeerLookup, {.peer = u"vinyl_ghost"_s});
    governor.setPaused(true);
    governor.request(Operation::PeerLookup, {.peer = u"vinyl_ghost"_s});
    EXPECT_EQ(count(), 0); // core::GovernorPermits records them, with their purpose
}

TEST_F(ActivityLogTest, TheRequestMemoryIsBounded)
{
    // A burst of requests to more peers than it keeps: it starts over, and the newest still names its lookup.
    log.recordDecision(Operation::Browse, u"first"_s, Refusal::None, QString());
    for (int i = 0; i < ActivityLog::kRequestMemoryPeers + 10; ++i) {
        log.recordDecision(Operation::Browse, u"peer%1"_s.arg(i), Refusal::None, QString());
    }
    log.recordPeerLookup(u"first"_s, ConnectPurpose::Request, Refusal::None, QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"one of your requests."_s));
    log.recordPeerLookup(u"peer%1"_s.arg(ActivityLog::kRequestMemoryPeers + 9), ConnectPurpose::Request, Refusal::None,
                         QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"browse their shares."_s));
}

TEST_F(ActivityLogTest, TheRequestMemoryForgetsTheOldestFirst)
{
    for (int i = 0; i < ActivityLog::kRequestMemoryPeers - 1; ++i) {
        log.recordDecision(Operation::Browse, u"old%1"_s.arg(i), Refusal::None, QString());
    }
    nowMs += ActivityLog::kRequestMemoryMs - 1000;
    log.recordDecision(Operation::UserInfo, u"fresh"_s, Refusal::None, QString()); // the memory is full now
    nowMs += 1500;                                                                 // the old ones are too old
    log.recordDecision(Operation::Browse, u"newcomer"_s, Refusal::None, QString());
    log.recordPeerLookup(u"fresh"_s, ConnectPurpose::Request, Refusal::None, QString());
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"ask for their user info."_s));
}

TEST_F(ActivityLogTest, OurRequestsSayWeAskedAndWhyTheyWereHeldBack)
{
    EXPECT_EQ(describe(u"browse"_s, u"x"_s, u"allowed"_s, u"Browsing: allowed."_s),
              u"You asked to browse x's shares."_s);
    EXPECT_EQ(describe(u"user-info"_s, u"x"_s, u"allowed"_s), u"You asked x for their user info."_s);
    EXPECT_EQ(describe(u"folder-contents"_s, u"x"_s, u"allowed"_s), u"You asked x what's in one of their folders."_s);
    EXPECT_EQ(describe(u"queue-download"_s, u"x"_s, u"allowed"_s, u"Download allowed."_s),
              u"You asked x to queue a download for you."_s);
    EXPECT_EQ(describe(u"place-in-queue"_s, u"x"_s, u"allowed"_s),
              u"You asked x where your download stands in their queue."_s);
    EXPECT_EQ(describe(u"private-message"_s, u"x"_s, u"allowed"_s), u"You sent x a private message."_s);

    EXPECT_EQ(describe(u"browse"_s, u"x"_s, u"rate-limited"_s, u"Browsing: retry in 5 s."_s),
              u"You wanted to browse x's shares. Selecta held it back: Browsing: retry in 5 s."_s);
    EXPECT_EQ(describe(u"queue-download"_s, u"x"_s, u"peer-queue-limit"_s, u"Too many files queued at x."_s),
              u"You wanted to ask x to queue a download for you. Selecta held it back: Too many files queued at x."_s);
    EXPECT_EQ(describe(u"private-message"_s, u"x"_s, u"message-repeated"_s),
              u"You wanted to send x a private message. Selecta held it back."_s);
}

TEST_F(ActivityLogTest, OlderLookupLinesStillSayWhoStartedThem)
{
    // Written before lookups carried their purpose: the governor's own words.
    EXPECT_EQ(describe(u"peer-lookup"_s, u"x"_s, u"allowed"_s, u"Peer lookups: allowed."_s),
              u"You looked up x's address to connect to them (this line is older than Selecta's notes on why)."_s);
    EXPECT_EQ(describe(u"peer-lookup"_s, u"x"_s, u"rate-limited"_s, u"Peer lookups: retry in 2 s."_s),
              u"You wanted to look up x's address. Selecta held it back: Peer lookups: retry in 2 s."_s);
}

TEST_F(ActivityLogTest, DownloadsAlbumsAndArrivalsAreOurs)
{
    EXPECT_EQ(describe(u"download"_s, u"x"_s, u"wanted"_s, u"song.flac"_s), u"You want song.flac from x."_s);
    EXPECT_EQ(describe(u"download"_s, u"x"_s, u"completed"_s, u"song.flac"_s), u"You downloaded song.flac from x."_s);
    EXPECT_EQ(describe(u"download"_s, u"x"_s, u"failed"_s, u"song.flac: Timed out"_s),
              u"Your download from x failed: song.flac: Timed out"_s);
    EXPECT_EQ(describe(u"download"_s, u"x"_s, u"from-preview"_s, u"song.flac"_s),
              u"Your preview of song.flac from x became the download: nothing more to fetch."_s);
    EXPECT_EQ(describe(u"download"_s, u"x"_s, u"kept-preview"_s, u"song.flac"_s),
              u"You kept the preview of song.flac from x: it carries on as a download."_s);
    EXPECT_EQ(describe(u"album"_s, u"x"_s, u"wanted"_s, u"Album (2001)"_s),
              u"You want the album Album (2001) from x."_s);
    EXPECT_EQ(describe(u"album"_s, u"x"_s, u"failed"_s, u"No audio."_s), u"Your album from x failed: No audio."_s);
    EXPECT_EQ(describe(u"album"_s, u"x"_s, u"cancelled"_s, u"Album"_s), u"You cancelled the album Album from x."_s);
    EXPECT_EQ(describe(u"album"_s, u"x"_s, u"arrived"_s, u"Inbox/Album"_s),
              u"Your album from x is in your Inbox: Inbox/Album"_s);
    EXPECT_EQ(describe(u"arrival"_s, u"x"_s, u"arrived"_s, u"Inbox/a.mp3"_s),
              u"Your file from x is in your Inbox: Inbox/a.mp3"_s);
    EXPECT_EQ(describe(u"arrival"_s, u"x"_s, u"failed"_s, u"a.flac: bad file"_s),
              u"Your file from x couldn't go to your Inbox: a.flac: bad file"_s);
}

TEST_F(ActivityLogTest, UploadsBrowsesAndMessagesAreTheirs)
{
    EXPECT_EQ(describe(u"shares"_s, u"x"_s, u"browsed"_s, u"x looked at what you share"_s),
              u"x is browsing your shares: they asked for the list of what you share."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"queued"_s, u"Music/a.flac"_s),
              u"x asked to download Music/a.flac from you; it waits in your upload queue."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"started"_s, u"Music/a.flac"_s),
              u"x is downloading Music/a.flac from you."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"completed"_s, u"Music/a.flac"_s),
              u"x downloaded Music/a.flac from you."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"cancelled"_s, u"Music/a.flac"_s),
              u"x's download of Music/a.flac from you was cancelled."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"declined"_s, u"Music/a.flac"_s),
              u"x no longer wanted Music/a.flac from you."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"failed"_s, u"Music/a.flac"_s),
              u"x's download of Music/a.flac from you failed."_s);
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"denied"_s, u"Banned"_s),
              u"x asked to download from you; Selecta said no: Banned"_s);
    EXPECT_EQ(describe(u"message"_s, u"x"_s, u"ignored"_s),
              u"x sent you a private message; you ignore them, so Selecta dropped it."_s);
}

TEST_F(ActivityLogTest, UserListChangesAreOurs)
{
    EXPECT_EQ(describe(u"user-list"_s, u"x"_s, u"buddy"_s), u"You added x to your buddies."_s);
    EXPECT_EQ(describe(u"user-list"_s, u"x"_s, u"ignored"_s), u"You ignore x now: their messages are dropped."_s);
    EXPECT_EQ(describe(u"user-list"_s, u"x"_s, u"banned"_s),
              u"You banned x: they can't browse or download from you."_s);
    EXPECT_EQ(describe(u"user-list"_s, u"x"_s, u"unlisted"_s), u"You took x off your lists."_s);
}

TEST_F(ActivityLogTest, OtherLinesKeepTheirExplanation)
{
    EXPECT_EQ(describe(u"search"_s, u"neon rain"_s, u"allowed"_s, u"Search sent."_s), u"Search sent."_s);
    EXPECT_EQ(describe(u"server"_s, QString(), u"connected"_s, u"Logged in."_s), u"Logged in."_s);
    EXPECT_EQ(describe(u"supersede"_s, u"x"_s, u"decided"_s, u"2 singles replaced"_s), u"2 singles replaced"_s);
    // An outcome these sentences don't know, or a line with no peer, keeps what was stored.
    EXPECT_EQ(describe(u"upload"_s, u"x"_s, u"something-new"_s, u"as stored"_s), u"as stored"_s);
    EXPECT_EQ(describe(u"browse"_s, QString(), u"allowed"_s, u"as stored"_s), u"as stored"_s);
    EXPECT_EQ(describe(u"made-up"_s, u"x"_s, u"ok"_s), QString());
}

TEST_F(ActivityLogTest, PeerNamesWithPercentSignsStayAsTheyAre)
{
    EXPECT_EQ(describe(u"upload"_s, u"100%1"_s, u"queued"_s, u"%2 a.flac"_s),
              u"100%1 asked to download %2 a.flac from you; it waits in your upload queue."_s);
    log.recordPeerLookup(u"%1%2"_s, ConnectPurpose::SearchResponse, Refusal::RateLimited, u"%3"_s);
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.startsWith(u"You wanted to look up %1%2's"_s));
    EXPECT_TRUE(ActivityLog::recent(db, 1).constFirst().explanation.endsWith(u"held it back: %3"_s));
}
