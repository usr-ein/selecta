// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/Governor.h"

#include "base/Clock.h"
#include "governor/Limits.h"

#include <QSignalSpy>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::governor::Governor;
using selecta::governor::Operation;
using selecta::governor::Refusal;
using selecta::governor::Request;
using selecta::governor::SearchSource;
using Event = selecta::governor::ReconnectPolicy::Event;
namespace limits = selecta::governor::limits;

namespace {

class GovernorTest : public ::testing::Test
{
protected:
    ManualClock clock;
    QDate day{2026, 10, 20};
    Governor governor{clock, {.today = [this] { return day; }, .random01 = [] { return 0.0; }}};

    Refusal search(const QString &query, SearchSource source = SearchSource::Manual)
    {
        return governor.request(Operation::Search, Request{.query = query, .source = source}).refusal;
    }
    Refusal queue(const QString &peer, bool preview = false)
    {
        return governor.request(Operation::QueueDownload, Request{.peer = peer, .preview = preview}).refusal;
    }
};

} // namespace

TEST_F(GovernorTest, SearchBudgetIsSixThenOneEveryThirtySeconds)
{
    for (int i = 0; i < limits::kSearchBurst; ++i) {
        ASSERT_EQ(search(u"query number %1"_s.arg(10 + i)), Refusal::None)
            << i; // two digits: one-letter words are dropped
    }
    const auto refused = governor.request(Operation::Search, Request{.query = u"one too many"_s});
    EXPECT_EQ(refused.refusal, Refusal::RateLimited);
    EXPECT_EQ(refused.retryAfter, 30s);
    EXPECT_FALSE(refused.allowed());
    clock.advance(30s);
    EXPECT_EQ(search(u"one too many"_s), Refusal::None);
}

TEST_F(GovernorTest, TheRefillIsTimedEvenWhileSearchesAreLeft)
{
    EXPECT_EQ(governor.nextSearchRefillIn(), 0s); // full: nothing to wait for
    ASSERT_EQ(search(u"query number 10"_s), Refusal::None);
    ASSERT_EQ(search(u"query number 11"_s), Refusal::None);
    EXPECT_EQ(governor.nextSearchIn(), 0s);        // a search can still go now…
    EXPECT_EQ(governor.nextSearchRefillIn(), 30s); // …and the next one comes back in 30 s
    clock.advance(10s);
    EXPECT_EQ(governor.nextSearchRefillIn(), 20s);
    clock.advance(20s);
    EXPECT_EQ(governor.searchesAvailable(), limits::kSearchBurst - 1);
    EXPECT_EQ(governor.nextSearchRefillIn(), 30s);
}

TEST_F(GovernorTest, AllowedSearchCarriesTheCleanedQuery)
{
    const auto decision = governor.request(Operation::Search, Request{.query = u"  neon   rain x "_s});
    if (!decision.permit) {
        ADD_FAILURE() << "expected a permit";
        return;
    }
    EXPECT_EQ(decision.cleanedQuery, u"neon rain"_s);
    EXPECT_EQ(decision.permit->operation(), Operation::Search);
}

TEST_F(GovernorTest, SameSearchWithinTenMinutesUsesTheCache)
{
    ASSERT_EQ(search(u"Neon Rain"_s), Refusal::None);
    EXPECT_EQ(search(u"rain  NEON"_s), Refusal::SameQueryRecently);
    EXPECT_EQ(search(u"rain neon"_s, SearchSource::Refresh), Refusal::SameQueryRecently); // refresh obeys too
    clock.advance(10min);
    EXPECT_EQ(search(u"neon rain"_s), Refusal::None);
}

TEST_F(GovernorTest, RefusedSearchesSpendNothing)
{
    EXPECT_EQ(search(u"ab"_s), Refusal::QueryTooShort);
    EXPECT_EQ(governor.searchesAvailable(), limits::kSearchBurst);
}

TEST_F(GovernorTest, GetTracksLeavesSearchesForTheUser)
{
    int sent = 0;
    while (search(u"track %1"_s.arg(10 + sent), SearchSource::GetTracks) == Refusal::None) {
        ++sent;
    }
    EXPECT_EQ(sent, limits::kSearchBurst - limits::kGetTracksReservedSearches);
    EXPECT_EQ(search(u"typed by hand"_s), Refusal::None);
}

TEST_F(GovernorTest, ExcludedPhrasesAreRefused)
{
    governor.setExcludedPhrases({u"forbidden words"_s});
    const auto decision = governor.request(Operation::Search, Request{.query = u"some forbidden words here"_s});
    EXPECT_EQ(decision.refusal, Refusal::QueryExcludedPhrase);
    EXPECT_TRUE(decision.explanation.contains(u"forbidden words"_s));
}

TEST_F(GovernorTest, PauseStopsSoulseekButNotMetadata)
{
    const QSignalSpy paused(&governor, &Governor::pausedChanged);
    governor.setPaused(true);
    governor.setPaused(true);
    EXPECT_EQ(paused.count(), 1);
    EXPECT_EQ(search(u"neon rain"_s), Refusal::NetworkPaused);
    EXPECT_EQ(queue(u"peer"_s), Refusal::NetworkPaused);
    EXPECT_TRUE(governor.request(Operation::SpotifyRequest).allowed());
    governor.setPaused(false);
    EXPECT_EQ(search(u"neon rain"_s), Refusal::None);
}

TEST_F(GovernorTest, PeerQueueAndDailyLimits)
{
    for (int i = 0; i < limits::kPeerQueuedFiles; ++i) {
        ASSERT_EQ(queue(u"peer"_s), Refusal::None);
        governor.downloadQueued(u"peer"_s);
    }
    EXPECT_EQ(queue(u"peer"_s), Refusal::PeerQueueLimit);
    EXPECT_EQ(queue(u"other"_s), Refusal::None);

    for (int i = 0; i < limits::kPeerDailyStopFiles; ++i) {
        governor.downloadStarted(u"busy"_s);
        governor.downloadFinished(u"busy"_s, 1'000'000, true);
    }
    EXPECT_EQ(queue(u"busy"_s), Refusal::PeerDailyLimit);
    day = day.addDays(1);
    EXPECT_EQ(queue(u"busy"_s), Refusal::None);
}

TEST_F(GovernorTest, DailyBytesStopToo)
{
    governor.downloadStarted(u"big"_s);
    governor.downloadFinished(u"big"_s, limits::kPeerDailyStopBytes, true);
    EXPECT_EQ(queue(u"big"_s), Refusal::PeerDailyLimit);
}

TEST_F(GovernorTest, WarnsNearTheDailySoftCap)
{
    for (int i = 0; i < limits::kPeerDailyWarnFiles - 1; ++i) {
        governor.downloadStarted(u"peer"_s);
        governor.downloadFinished(u"peer"_s, 1, true);
    }
    const auto decision = governor.request(Operation::QueueDownload, Request{.peer = u"peer"_s});
    ASSERT_TRUE(decision.allowed());
    EXPECT_FALSE(decision.warning.isEmpty());
}

TEST_F(GovernorTest, FailedDownloadsDontCount)
{
    governor.downloadStarted(u"peer"_s);
    governor.downloadFinished(u"peer"_s, 5'000'000, false);
    EXPECT_EQ(governor.peerToday(u"peer"_s).files, 0);
}

TEST_F(GovernorTest, DeniedPeerIsLeftAlone)
{
    governor.peerDenied(u"peer"_s, u"Banned"_s);
    const auto decision = governor.request(Operation::QueueDownload, Request{.peer = u"peer"_s});
    EXPECT_EQ(decision.refusal, Refusal::PeerBackoff);
    EXPECT_GT(decision.retryAfter, std::chrono::milliseconds(std::chrono::days(29)));
}

TEST_F(GovernorTest, PreviewsWaitWhileDownloadingFromThatPeer)
{
    governor.downloadStarted(u"peer"_s);
    EXPECT_EQ(queue(u"peer"_s, true), Refusal::PeerActiveLimit);
    EXPECT_EQ(queue(u"peer"_s, false), Refusal::None);
    governor.downloadFinished(u"peer"_s, 1, true);
    EXPECT_EQ(queue(u"peer"_s, true), Refusal::None);
}

TEST_F(GovernorTest, PreviewsArePaced)
{
    for (int i = 0; i < limits::kPreviewsPer10Min; ++i) {
        ASSERT_EQ(queue(u"peer%1"_s.arg(i), true), Refusal::None) << i;
    }
    EXPECT_EQ(queue(u"another"_s, true), Refusal::RateLimited);
    EXPECT_EQ(queue(u"another"_s, false), Refusal::None); // ordinary downloads aren't preview-paced
}

TEST_F(GovernorTest, DownloadSlots)
{
    for (int i = 0; i < limits::kActiveDownloads; ++i) {
        EXPECT_TRUE(governor.downloadSlotFree());
        governor.downloadStarted(u"peer%1"_s.arg(i));
    }
    EXPECT_FALSE(governor.downloadSlotFree());
    governor.downloadFinished(u"peer0"_s, 1, true);
    EXPECT_TRUE(governor.downloadSlotFree());
}

TEST_F(GovernorTest, LoginRefusesTheLiveNetworkAndRespectsBans)
{
    const auto live = governor.request(Operation::Login, Request{.server = QHostAddress(u"8.8.8.8"_s)});
    EXPECT_EQ(live.refusal, Refusal::LiveServerRefused);

    const QHostAddress local(u"127.0.0.1"_s);
    EXPECT_TRUE(governor.request(Operation::Login, Request{.server = local}).allowed());

    governor.serverEvent(Event::BanNotice);
    EXPECT_TRUE(governor.isPaused());
    EXPECT_TRUE(governor.loginNeedsUser());
    governor.setPaused(false);
    EXPECT_EQ(governor.request(Operation::Login, Request{.server = local}).refusal, Refusal::LoginNeedsUser);
}

TEST_F(GovernorTest, RepeatedMessagesAreRefused)
{
    const Request hello{.recipient = u"friend"_s, .text = u"hello there"_s};
    EXPECT_TRUE(governor.request(Operation::PrivateMessage, hello).allowed());
    clock.advance(3s);
    EXPECT_EQ(governor.request(Operation::PrivateMessage, hello).refusal, Refusal::MessageRepeated);
    EXPECT_TRUE(
        governor.request(Operation::PrivateMessage, Request{.recipient = u"friend"_s, .text = u"bye"_s}).allowed());
}

TEST_F(GovernorTest, MessagesAreSpacedTwoSecondsApart)
{
    EXPECT_TRUE(governor.request(Operation::PrivateMessage, Request{.recipient = u"a"_s, .text = u"one"_s}).allowed());
    EXPECT_EQ(governor.request(Operation::PrivateMessage, Request{.recipient = u"b"_s, .text = u"two"_s}).refusal,
              Refusal::RateLimited);
}

TEST_F(GovernorTest, SameUserIsntBrowsedAgainSoon)
{
    EXPECT_TRUE(governor.request(Operation::Browse, Request{.peer = u"Peer"_s}).allowed());
    EXPECT_EQ(governor.request(Operation::Browse, Request{.peer = u"peer"_s}).refusal, Refusal::PeerTooSoon);
    clock.advance(10min);
    EXPECT_TRUE(governor.request(Operation::Browse, Request{.peer = u"peer"_s}).allowed());
}

TEST_F(GovernorTest, EveryDecisionIsReported)
{
    const QSignalSpy decided(&governor, &Governor::decided);
    search(u"neon rain"_s);
    search(u"ab"_s);
    governor.request(Operation::MusicBrainzRequest);
    ASSERT_EQ(decided.count(), 3);
    EXPECT_EQ(decided.at(1).at(2).value<Refusal>(), Refusal::QueryTooShort);
}

TEST_F(GovernorTest, PermitsAreSingleUse)
{
    auto decision = governor.request(Operation::MusicBrainzRequest);
    if (!decision.permit) {
        ADD_FAILURE() << "expected a permit";
        return;
    }
    selecta::governor::Permit permit = std::move(*decision.permit);
    EXPECT_TRUE(permit.consume(Operation::MusicBrainzRequest));
    EXPECT_FALSE(permit.isValid());
}
