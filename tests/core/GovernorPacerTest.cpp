// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GovernorPacer.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QCoreApplication>
#include <QObject>

#include <gtest/gtest.h>
#include <memory>
#include <optional>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::ActivityLog;
using selecta::core::GovernorPacer;
using selecta::governor::Operation;
using selecta::meta::PermitDecision;

namespace {

class GovernorPacerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        QObject::connect(&governor, &selecta::governor::Governor::decided, &log, &ActivityLog::recordDecision);
    }

    // Asks once and returns the answer after letting queued calls run.
    std::optional<PermitDecision> ask(Operation operation)
    {
        std::optional<PermitDecision> answer;
        pacer.acquire(operation, [&answer](PermitDecision decision) { answer = std::move(decision); });
        EXPECT_FALSE(answer.has_value()) << "the grant must never arrive inside acquire()";
        QCoreApplication::processEvents();
        return answer;
    }

    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    selecta::storage::Database db;
    ActivityLog log{db, [] { return qint64{1'800'000'000'000}; }};
    QObject context;
    GovernorPacer pacer{governor, log, &context};
};

} // namespace

TEST_F(GovernorPacerTest, GrantsAPermitForTheOperationOnALaterTurn)
{
    auto answer = ask(Operation::SpotifyRequest);
    ASSERT_TRUE(answer.has_value());
    ASSERT_TRUE(answer->permit.has_value());
    EXPECT_EQ(answer->permit->operation(), Operation::SpotifyRequest);
    EXPECT_TRUE(answer->refusal.isEmpty());
}

TEST_F(GovernorPacerTest, ARefusalSaysWhyAndWhenToAskAgain)
{
    // MusicBrainz allows one request at a time, then one per second.
    ASSERT_TRUE(ask(Operation::MusicBrainzRequest)->permit.has_value());
    const auto refused = ask(Operation::MusicBrainzRequest);
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->permit.has_value());
    EXPECT_FALSE(refused->refusal.isEmpty());
    EXPECT_GT(refused->retryAfter, 0ms);
    clock.advance(refused->retryAfter);
    EXPECT_TRUE(ask(Operation::MusicBrainzRequest)->permit.has_value());
}

TEST_F(GovernorPacerTest, PausingTheNetworkDoesntStopMetadata)
{
    governor.setPaused(true);
    EXPECT_TRUE(ask(Operation::DeezerRequest)->permit.has_value());
}

TEST_F(GovernorPacerTest, NoGrantOnceTheContextIsGone)
{
    auto pacerContext = std::make_unique<QObject>();
    GovernorPacer shortLived(governor, log, pacerContext.get());
    bool called = false;
    shortLived.acquire(Operation::SpotifyRequest, [&called](const PermitDecision &) { called = true; });
    pacerContext.reset();
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(GovernorPacerTest, ReportsGoToTheActivityLogAndAllowedDecisionsDont)
{
    ask(Operation::SpotifyRequest); // allowed: no line until the request reports
    EXPECT_TRUE(ActivityLog::recent(db, 10).isEmpty());

    pacer.report(selecta::meta::RequestRecord{.service = selecta::meta::Service::SpotifyApi,
                                              .method = "GET",
                                              .target = u"api.spotify.com/v1/search?q=neon+rain&type=track"_s,
                                              .httpStatus = 200,
                                              .bytes = 2048,
                                              .elapsed = 340ms,
                                              .outcome = u"ok"_s});
    const auto lines = ActivityLog::recent(db, 10);
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].operation, u"spotify"_s);
    EXPECT_EQ(lines[0].subject, u"GET api.spotify.com/v1/search?q=neon+rain&type=track"_s);
    EXPECT_EQ(lines[0].outcome, u"ok"_s);
    EXPECT_TRUE(lines[0].explanation.contains(u"HTTP 200"_s));
    EXPECT_TRUE(lines[0].explanation.contains(u"340 ms"_s));
}

TEST_F(GovernorPacerTest, RefusalsAreLogged)
{
    ask(Operation::MusicBrainzRequest);
    ask(Operation::MusicBrainzRequest); // refused
    const auto lines = ActivityLog::recent(db, 10);
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].operation, u"musicbrainz"_s);
    EXPECT_EQ(lines[0].outcome, u"rate-limited"_s);
}
