// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/PeerLedger.h"

#include "base/Clock.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::governor::PeerDay;
using selecta::governor::PeerLedger;

namespace {

class PeerLedgerTest : public ::testing::Test
{
protected:
    ManualClock clock;
    QDate day{2026, 10, 20};
    PeerLedger ledger{clock, [this] { return day; }};
};

} // namespace

TEST_F(PeerLedgerTest, CountsTodayAndRollsOverAtMidnight)
{
    ledger.recordCompleted(u"vinyl_ghost"_s, 10'000'000);
    ledger.recordCompleted(u"vinyl_ghost"_s, 5'000'000);
    EXPECT_EQ(ledger.today(u"vinyl_ghost"_s).files, 2);
    EXPECT_EQ(ledger.today(u"vinyl_ghost"_s).bytes, 15'000'000);
    day = day.addDays(1);
    EXPECT_EQ(ledger.today(u"vinyl_ghost"_s).files, 0);
    EXPECT_EQ(ledger.filesLastSevenDays(u"vinyl_ghost"_s), 2);
}

TEST_F(PeerLedgerTest, SevenDayWindowSlides)
{
    for (int i = 0; i < 8; ++i) {
        ledger.recordCompleted(u"peer"_s, 1);
        day = day.addDays(1);
    }
    EXPECT_EQ(ledger.filesLastSevenDays(u"peer"_s), 6); // today is empty; the six days before it have one each
}

TEST_F(PeerLedgerTest, NegativeSizesDontReduceTotals)
{
    ledger.recordCompleted(u"peer"_s, -50);
    EXPECT_EQ(ledger.today(u"peer"_s).bytes, 0);
    EXPECT_EQ(ledger.today(u"peer"_s).files, 1);
}

TEST_F(PeerLedgerTest, RestoringKeepsTheLargerTotals)
{
    ledger.recordCompleted(u"peer"_s, 100);
    ledger.restoreDay(u"peer"_s, day, PeerDay{.files = 7, .bytes = 50});
    EXPECT_EQ(ledger.today(u"peer"_s).files, 7);
    EXPECT_EQ(ledger.today(u"peer"_s).bytes, 100);
    ledger.restoreDay(u"peer"_s, day.addDays(-30), PeerDay{.files = 99, .bytes = 1}); // too old: ignored
    ledger.restoreDay(u"peer"_s, QDate(), PeerDay{.files = 99, .bytes = 1});          // invalid: ignored
    EXPECT_EQ(ledger.filesLastSevenDays(u"peer"_s), 7);
}

TEST_F(PeerLedgerTest, QueueCountersNeverGoNegative)
{
    ledger.queued(u"a"_s);
    ledger.queued(u"a"_s);
    ledger.started(u"a"_s);
    EXPECT_EQ(ledger.queuedAt(u"a"_s), 1);
    EXPECT_EQ(ledger.activeAt(u"a"_s), 1);
    ledger.finished(u"a"_s);
    ledger.finished(u"a"_s); // a stray event
    ledger.withdrawn(u"a"_s);
    ledger.withdrawn(u"a"_s); // a stray event
    EXPECT_EQ(ledger.queuedAt(u"a"_s), 0);
    EXPECT_EQ(ledger.activeAt(u"a"_s), 0);
    EXPECT_EQ(ledger.queuedTotal(), 0);
    EXPECT_EQ(ledger.activeTotal(), 0);
}

TEST_F(PeerLedgerTest, BanDenialBacksOffThirtyDays)
{
    EXPECT_EQ(ledger.denied(u"p"_s, u"Banned"_s), std::chrono::milliseconds(std::chrono::days(30)));
    EXPECT_TRUE(ledger.backoffRemaining(u"p"_s).has_value());
    EXPECT_TRUE(ledger.denied(u"q"_s, u"You are banned from this user"_s).has_value());
    clock.advance(std::chrono::days(30));
    EXPECT_FALSE(ledger.backoffRemaining(u"p"_s).has_value());
}

TEST_F(PeerLedgerTest, FileNotSharedOnceIsForgivenTwiceBacksOff)
{
    EXPECT_FALSE(ledger.denied(u"p"_s, u"File not shared."_s).has_value());
    clock.advance(1h);
    EXPECT_EQ(ledger.denied(u"p"_s, u"File not shared."_s), std::chrono::milliseconds(24h));
    clock.advance(25h);
    EXPECT_FALSE(ledger.denied(u"p"_s, u"File not shared."_s).has_value()); // the previous one is outside the window
}

TEST_F(PeerLedgerTest, ShutdownAndRetryableDenials)
{
    EXPECT_EQ(ledger.denied(u"p"_s, u"Pending shutdown."_s), std::chrono::milliseconds(15min));
    EXPECT_FALSE(ledger.denied(u"q"_s, u"Too many files"_s).has_value());
    EXPECT_FALSE(ledger.denied(u"q"_s, u"Queued"_s).has_value());
    EXPECT_FALSE(ledger.backoffRemaining(u"q"_s).has_value());
}

TEST_F(PeerLedgerTest, LongerBackoffWins)
{
    ledger.denied(u"p"_s, u"Banned"_s);
    ledger.denied(u"p"_s, u"Pending shutdown."_s);
    const auto remaining = ledger.backoffRemaining(u"p"_s);
    if (!remaining) {
        ADD_FAILURE() << "expected a back-off";
        return;
    }
    EXPECT_GT(*remaining, std::chrono::milliseconds(std::chrono::days(29)));
}
