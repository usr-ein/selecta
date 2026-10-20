// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Notifications.h"

#include "core/AlbumService.h"
#include "core/Albums.h"
#include "core/GetTracks.h"
#include "core/RoomService.h"
#include "core/SocialService.h"
#include "core/WishlistService.h"
#include "support/FakeNotificationSink.h"

#include <QSignalSpy>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::FinishedBatch;
using selecta::app::GetTracksBatch;
using selecta::app::NotificationTarget;
using selecta::app::Notifier;
using selecta::app::SystemNotification;
using selecta::core::GetLine;
using DownloadState = selecta::core::GetLine::DownloadState;

namespace {

GetLine line(qint64 id, const QString &artist, const QString &title, DownloadState state)
{
    GetLine line;
    line.id = id;
    line.artist = artist;
    line.title = title;
    line.raw = artist + u" - "_s + title;
    line.downloadState = state;
    return line;
}

selecta::core::ChatMessage message(const QString &peer, const QString &text)
{
    return {.id = 1, .peer = peer, .atMs = 0, .outgoing = false, .text = text, .read = false};
}

class NotifierTest : public ::testing::Test
{
protected:
    selecta::testing::FakeNotificationSink sink;
    bool away = true;
    Notifier notifier{&sink, [this] { return away; }};
};

} // namespace

TEST_F(NotifierTest, AMessageReachesTheSystemOnlyWhileTheUserIsAway)
{
    away = false;
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Got the B-side?"_s));
    EXPECT_TRUE(sink.posted.isEmpty()); // the window's own notification is enough

    away = true;
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Got the B-side?"_s));
    ASSERT_EQ(sink.posted.size(), 1);
    const SystemNotification &posted = sink.posted.constFirst();
    EXPECT_EQ(posted.title, u"Message from vinyl_ghost"_s);
    EXPECT_EQ(posted.body, u"Got the B-side?"_s);
    EXPECT_EQ(posted.target, (NotificationTarget{u"messages"_s, u"vinyl_ghost"_s}));
    EXPECT_EQ(posted.thread, posted.id);
    EXPECT_TRUE(posted.sound); // someone is waiting for an answer

    // Our own messages never notify.
    auto own = message(u"vinyl_ghost"_s, u"Yes, 24/96"_s);
    own.outgoing = true;
    notifier.messageAdded(own);
    EXPECT_EQ(sink.posted.size(), 1);
}

TEST_F(NotifierTest, MessagesFromOnePeerCoalesceUntilTheUserIsBack)
{
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Got the B-side?"_s));
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Or the dub?"_s));
    notifier.messageAdded(message(u"lossless_lena"_s, u"Hi"_s));
    ASSERT_EQ(sink.posted.size(), 3);
    // The second replaces the first in Notification Center, with the count and quietly.
    EXPECT_EQ(sink.posted.at(1).id, sink.posted.at(0).id);
    EXPECT_EQ(sink.posted.at(1).title, u"2 messages from vinyl_ghost"_s);
    EXPECT_EQ(sink.posted.at(1).body, u"Or the dub?"_s);
    EXPECT_FALSE(sink.posted.at(1).sound);
    EXPECT_NE(sink.posted.at(2).id, sink.posted.at(0).id); // someone else: a conversation of its own

    notifier.userReturned();
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Still there?"_s));
    EXPECT_EQ(sink.posted.constLast().title, u"Message from vinyl_ghost"_s);
    EXPECT_TRUE(sink.posted.constLast().sound);
}

TEST_F(NotifierTest, NothingReachesTheSystemWhenTurnedOff)
{
    notifier.setEnabled(false);
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Got the B-side?"_s));
    notifier.roomLineAdded(u"dnb"_s, {.atMs = 0, .user = u"lena"_s, .text = u"me: hi"_s, .own = false, .mention = true});
    notifier.wishlistFound({.id = 3, .text = u"Okuma Drift"_s, .createdAt = 0, .lastRunMs = 0, .found = 2}, 2);
    EXPECT_TRUE(sink.posted.isEmpty());

    // The Dock badge still counts what arrives.
    notifier.getLineChanged(line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Queued));
    notifier.getLineChanged(line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Arrived));
    EXPECT_EQ(notifier.badge(), 1);
    EXPECT_TRUE(sink.posted.isEmpty());

    notifier.setEnabled(true);
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"Got the B-side?"_s));
    EXPECT_EQ(sink.posted.size(), 1);
}

TEST_F(NotifierTest, BeingNamedInARoomNotifiesWithTheRoom)
{
    notifier.roomLineAdded(u"dnb"_s, {.atMs = 0, .user = u"lena"_s, .text = u"hello all"_s, .own = false, .mention = false});
    notifier.roomLineAdded(u"dnb"_s, {.atMs = 0, .user = u"me"_s, .text = u"me again"_s, .own = true, .mention = true});
    EXPECT_TRUE(sink.posted.isEmpty());

    notifier.roomLineAdded(u"dnb"_s, {.atMs = 0,
                                      .user = u"lossless_lena"_s,
                                      .text = u"selecta_test: got the B-side?"_s,
                                      .own = false,
                                      .mention = true});
    ASSERT_EQ(sink.posted.size(), 1);
    EXPECT_EQ(sink.posted.constFirst().title, u"lossless_lena mentioned you in dnb"_s);
    EXPECT_EQ(sink.posted.constFirst().body, u"selecta_test: got the B-side?"_s);
    EXPECT_EQ(sink.posted.constFirst().target, (NotificationTarget{u"rooms"_s, u"dnb"_s}));
    EXPECT_TRUE(sink.posted.constFirst().sound);

    notifier.roomLineAdded(u"dnb"_s, {.atMs = 0,
                                      .user = u"kassia"_s,
                                      .text = u"selecta_test: and the A-side"_s,
                                      .own = false,
                                      .mention = true});
    ASSERT_EQ(sink.posted.size(), 2);
    EXPECT_EQ(sink.posted.constLast().id, sink.posted.constFirst().id);
    EXPECT_EQ(sink.posted.constLast().title, u"2 mentions of you in dnb"_s);
    EXPECT_EQ(sink.posted.constLast().body, u"kassia: selecta_test: and the A-side"_s);
}

TEST_F(NotifierTest, AlbumsThatArriveOrFailNotifyOnce)
{
    selecta::core::AlbumReport report;
    report.albumId = 4;
    report.album = u"Night Signals"_s;
    report.arrival = true;
    report.complete = true;
    report.tracks = 9;
    report.where = u"Albums/Okuma_Drift_-_Night_Signals_(2024)"_s;
    notifier.albumReported(report);
    notifier.albumReported(report); // told once
    ASSERT_EQ(sink.posted.size(), 1);
    EXPECT_EQ(sink.posted.constFirst().title, u"“Night Signals” is in your Inbox"_s);
    EXPECT_EQ(sink.posted.constFirst().body, u"9 tracks in Inbox/Albums/Okuma_Drift_-_Night_Signals_(2024)/"_s);
    EXPECT_EQ(sink.posted.constFirst().target, (NotificationTarget{u"inbox"_s, {}}));
    EXPECT_FALSE(sink.posted.constFirst().sound);
    EXPECT_EQ(notifier.arrivalsWhileAway(), 1);

    // Singles moved to the Trash on the user's word: they're here for that.
    report.albumId = 8;
    report.arrival = false;
    report.trashed = {u"A – One"_s};
    notifier.albumReported(report);
    EXPECT_EQ(sink.posted.size(), 1);

    report.albumId = 5;
    report.album = u"Harbour Lights"_s;
    report.arrival = true;
    report.complete = false;
    report.tracks = 1;
    report.where = u"Albums/Kassia_Moreau_-_Harbour_Lights"_s;
    report.warning = u"1 file of the album couldn't be downloaded or converted."_s;
    notifier.albumReported(report);
    ASSERT_EQ(sink.posted.size(), 2);
    EXPECT_EQ(sink.posted.constLast().title, u"“Harbour Lights” arrived incomplete"_s);
    EXPECT_EQ(sink.posted.constLast().body,
              u"1 track in Inbox/Albums/Kassia_Moreau_-_Harbour_Lights/\n"_s + report.warning);
    EXPECT_EQ(notifier.arrivalsWhileAway(), 2);

    selecta::core::AlbumDownload album;
    album.id = 6;
    album.folder = "Music\\Kassia Moreau - Harbour Lights";
    album.state = selecta::core::AlbumDownload::State::Downloading;
    notifier.albumChanged(album); // downloading isn't news
    EXPECT_EQ(sink.posted.size(), 2);
    album.state = selecta::core::AlbumDownload::State::Failed;
    album.message = u"The folder holds no audio file."_s;
    notifier.albumChanged(album);
    notifier.albumChanged(album);
    ASSERT_EQ(sink.posted.size(), 3);
    EXPECT_EQ(sink.posted.constLast().title, u"Couldn't download “Kassia Moreau - Harbour Lights”"_s);
    EXPECT_EQ(sink.posted.constLast().body, album.message);
    EXPECT_EQ(sink.posted.constLast().target, (NotificationTarget{u"activity"_s, {}}));
    EXPECT_EQ(notifier.arrivalsWhileAway(), 2); // a failure isn't an arrival
}

TEST_F(NotifierTest, AnAlbumArrivingWhileTheUserIsHereIsntCounted)
{
    away = false;
    selecta::core::AlbumReport report;
    report.albumId = 4;
    report.album = u"Night Signals"_s;
    report.arrival = true;
    report.complete = true;
    notifier.albumReported(report);
    EXPECT_TRUE(sink.posted.isEmpty());
    EXPECT_EQ(notifier.badge(), 0);
    away = true;
    notifier.albumReported(report); // already told: the user saw it
    EXPECT_TRUE(sink.posted.isEmpty());
}

TEST_F(NotifierTest, TheWishlistAddsUpWhatItFindsWhileTheUserIsAway)
{
    const selecta::core::WishlistItem item{
        .id = 3, .text = u"Okuma Drift Neon Rain"_s, .createdAt = 0, .lastRunMs = 0, .found = 0};
    notifier.wishlistFound(item, 0);
    EXPECT_TRUE(sink.posted.isEmpty());
    notifier.wishlistFound(item, 2);
    ASSERT_EQ(sink.posted.size(), 1);
    EXPECT_EQ(sink.posted.constFirst().title, u"Wishlist: 2 new files for “Okuma Drift Neon Rain”"_s);
    EXPECT_EQ(sink.posted.constFirst().target, (NotificationTarget{u"wishlist"_s, {}}));
    notifier.wishlistFound(item, 1);
    ASSERT_EQ(sink.posted.size(), 2);
    EXPECT_EQ(sink.posted.constLast().id, sink.posted.constFirst().id);
    EXPECT_EQ(sink.posted.constLast().title, u"Wishlist: 3 new files for “Okuma Drift Neon Rain”"_s);
}

TEST_F(NotifierTest, TheBadgeCountsUnreadMessagesAndArrivalsWhileAway)
{
    const QSignalSpy changed(&notifier, &Notifier::badgeChanged);
    notifier.setUnreadMessages(2);
    EXPECT_EQ(notifier.badge(), 2);
    ASSERT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.constLast().at(0).toInt(), 2);
    notifier.setUnreadMessages(2);
    EXPECT_EQ(changed.count(), 1); // no change, no signal

    // Two lines arrive while the user is away, one while they're here.
    notifier.getLinesReset({line(1, u"A"_s, u"One"_s, DownloadState::Queued),
                            line(2, u"B"_s, u"Two"_s, DownloadState::Transferring),
                            line(3, u"C"_s, u"Three"_s, DownloadState::Converting)});
    notifier.getLineChanged(line(1, u"A"_s, u"One"_s, DownloadState::Arrived));
    notifier.getLineChanged(line(2, u"B"_s, u"Two"_s, DownloadState::Arrived));
    EXPECT_EQ(notifier.arrivalsWhileAway(), 2);
    EXPECT_EQ(notifier.badge(), 4);
    away = false;
    notifier.getLineChanged(line(3, u"C"_s, u"Three"_s, DownloadState::Arrived));
    EXPECT_EQ(notifier.badge(), 4);

    // Back: the arrivals have been seen; unread messages stay until read.
    notifier.userReturned();
    EXPECT_EQ(notifier.arrivalsWhileAway(), 0);
    EXPECT_EQ(notifier.badge(), 2);
    EXPECT_EQ(changed.constLast().at(0).toInt(), 2);
    notifier.setUnreadMessages(-3);
    EXPECT_EQ(notifier.badge(), 0);
}

TEST_F(NotifierTest, AFinishedGetTracksBatchNotifiesWhileAwayAndIsAlwaysReported)
{
    QSignalSpy finished(&notifier, &Notifier::getTracksFinished);
    notifier.getLinesReset({line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Queued),
                            line(2, u"Kassia Moreau"_s, u"Harbour Lights"_s, DownloadState::Queued)});
    GetLine first = line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Transferring);
    first.downloadProgress = 0.4;
    notifier.getLineChanged(first); // progress isn't news
    notifier.getLineChanged(line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Arrived));
    EXPECT_EQ(finished.count(), 0);
    EXPECT_TRUE(sink.posted.isEmpty());
    notifier.getLineChanged(line(2, u"Kassia Moreau"_s, u"Harbour Lights"_s, DownloadState::Failed));
    ASSERT_EQ(finished.count(), 1);
    const auto batch = finished.constFirst().at(0).value<FinishedBatch>();
    EXPECT_EQ(batch.arrived, 1);
    EXPECT_EQ(batch.failed, 1);
    ASSERT_EQ(sink.posted.size(), 1);
    EXPECT_EQ(sink.posted.constFirst().title, u"Get tracks finished"_s);
    EXPECT_EQ(sink.posted.constFirst().body, u"1 track is in your Inbox; 1 couldn't be downloaded."_s);
    EXPECT_EQ(sink.posted.constFirst().target, (NotificationTarget{u"inbox"_s, {}}));

    // With the user here, the batch is still reported (the window says it), but not to the system.
    away = false;
    notifier.getLineChanged(line(3, u"A"_s, u"One"_s, DownloadState::Queued));
    notifier.getLineChanged(line(3, u"A"_s, u"One"_s, DownloadState::Arrived));
    EXPECT_EQ(finished.count(), 2);
    EXPECT_EQ(sink.posted.size(), 1);

    // Each batch is news of its own.
    away = true;
    notifier.getLineChanged(line(4, u"B"_s, u"Two"_s, DownloadState::Queued));
    notifier.getLineChanged(line(4, u"B"_s, u"Two"_s, DownloadState::Arrived));
    ASSERT_EQ(sink.posted.size(), 2);
    EXPECT_NE(sink.posted.constLast().id, sink.posted.constFirst().id);
    EXPECT_EQ(sink.posted.constLast().body, u"“B – Two” is in your Inbox."_s);
}

TEST(GetTracksBatchTest, ABatchEndsWhenNothingIsOnItsWay)
{
    GetTracksBatch batch;
    EXPECT_FALSE(batch.reset({line(1, u"A"_s, u"One"_s, DownloadState::Queued),
                              line(2, u"B"_s, u"Two"_s, DownloadState::Queued),
                              line(3, u"C"_s, u"Three"_s, DownloadState::None)})
                     .finished);
    EXPECT_EQ(batch.pending(), 2);
    GetTracksBatch::Step step = batch.update(line(1, u"A"_s, u"One"_s, DownloadState::Arrived));
    EXPECT_EQ(step.arrived, 1);
    EXPECT_FALSE(step.finished);
    // A line sent while the batch runs joins it.
    EXPECT_FALSE(batch.update(line(3, u"C"_s, u"Three"_s, DownloadState::Queued)).finished);
    EXPECT_FALSE(batch.update(line(2, u"B"_s, u"Two"_s, DownloadState::Arrived)).finished);
    step = batch.update(line(3, u"C"_s, u"Three"_s, DownloadState::Failed));
    ASSERT_TRUE(step.finished);
    EXPECT_EQ(step.arrived, 0);
    EXPECT_EQ(step.finished->arrived, 2);
    EXPECT_EQ(step.finished->failed, 1);
    EXPECT_TRUE(step.finished->track.isEmpty());
    EXPECT_EQ(batch.pending(), 0);

    // Once over, the same lines changing again start nothing.
    EXPECT_FALSE(batch.update(line(1, u"A"_s, u"One"_s, DownloadState::Arrived)).finished);
    EXPECT_FALSE(batch.update(line(3, u"C"_s, u"Three"_s, DownloadState::None)).finished);
}

TEST(GetTracksBatchTest, CancelledAndRemovedLinesLeaveWithoutCounting)
{
    GetTracksBatch batch;
    // Everything cancelled: no news.
    EXPECT_FALSE(batch.update(line(1, u"A"_s, u"One"_s, DownloadState::Queued)).finished);
    EXPECT_FALSE(batch.update(line(1, u"A"_s, u"One"_s, DownloadState::None)).finished);

    // One cancelled, one arrived: the batch was that one track.
    batch.update(line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Queued));
    batch.update(line(2, u"B"_s, u"Two"_s, DownloadState::Queued));
    EXPECT_FALSE(batch.update(line(2, u"B"_s, u"Two"_s, DownloadState::None)).finished);
    const GetTracksBatch::Step step = batch.update(line(1, u"Okuma Drift"_s, u"Neon Rain"_s, DownloadState::Arrived));
    ASSERT_TRUE(step.finished);
    EXPECT_EQ(step.finished->arrived, 1);
    EXPECT_EQ(step.finished->track, u"Okuma Drift – Neon Rain"_s);

    // A line removed from the list while on its way leaves; the rest of the batch finishes without it.
    batch.update(line(3, u"C"_s, u"Three"_s, DownloadState::Queued));
    batch.update(line(4, u"D"_s, u"Four"_s, DownloadState::Queued));
    EXPECT_FALSE(batch.update(line(3, u"C"_s, u"Three"_s, DownloadState::Arrived)).finished);
    const GetTracksBatch::Step removed = batch.reset({line(3, u"C"_s, u"Three"_s, DownloadState::Arrived)});
    ASSERT_TRUE(removed.finished);
    EXPECT_EQ(removed.finished->arrived, 1);
    EXPECT_EQ(removed.finished->failed, 0);
    EXPECT_EQ(removed.arrived, 0); // counted when it arrived, not again

    // The whole list cleared mid-batch: nothing to tell.
    batch.update(line(5, u"E"_s, u"Five"_s, DownloadState::Transferring));
    EXPECT_FALSE(batch.reset({}).finished);
    EXPECT_EQ(batch.pending(), 0);
}

TEST(GetTracksBatchTest, LinesThatArrivedBeforeDontCountButResumingOnesDo)
{
    GetTracksBatch batch;
    // At start-up: one line from an earlier run already in the Inbox, one download resuming.
    GetTracksBatch::Step step = batch.reset({line(1, u"A"_s, u"One"_s, DownloadState::Arrived),
                                             line(2, u"B"_s, u"Two"_s, DownloadState::Transferring),
                                             line(3, u"C"_s, u"Three"_s, DownloadState::Failed)});
    EXPECT_EQ(step.arrived, 0);
    EXPECT_FALSE(step.finished);
    step = batch.update(line(2, u"B"_s, u"Two"_s, DownloadState::Arrived));
    EXPECT_EQ(step.arrived, 1);
    ASSERT_TRUE(step.finished);
    EXPECT_EQ(step.finished->arrived, 1);
    EXPECT_EQ(step.finished->failed, 0);
    EXPECT_EQ(step.finished->track, u"B – Two"_s);
}

TEST(GetTracksBatchTest, ALineNamesItsReleaseFirst)
{
    GetTracksBatch batch;
    GetLine pasted = line(1, u"okuma drift"_s, u"neon rain"_s, DownloadState::Queued);
    selecta::meta::Release release;
    release.artists = {u"Okuma Drift"_s, u"Kassia Moreau"_s};
    release.title = u"Neon Rain (Dub)"_s;
    pasted.release = release;
    batch.update(pasted);
    pasted.downloadState = DownloadState::Failed;
    const GetTracksBatch::Step step = batch.update(pasted);
    ASSERT_TRUE(step.finished);
    EXPECT_EQ(step.finished->track, u"Okuma Drift, Kassia Moreau – Neon Rain (Dub)"_s);

    GetLine raw;
    raw.id = 2;
    raw.raw = u"  ID - ID  "_s;
    raw.downloadState = DownloadState::Queued;
    batch.update(raw);
    raw.downloadState = DownloadState::Arrived;
    EXPECT_EQ(batch.update(raw).finished->track, u"ID - ID"_s);
}

TEST(NotifierWordsTest, AFinishedBatchSaysWhatArrivedAndWhatDidnt)
{
    SystemNotification words = Notifier::batchNotification({.arrived = 12, .failed = 0, .track = {}});
    EXPECT_EQ(words.title, u"Get tracks finished"_s);
    EXPECT_EQ(words.body, u"12 tracks are in your Inbox."_s);
    EXPECT_EQ(words.target, (NotificationTarget{u"inbox"_s, {}}));
    EXPECT_FALSE(words.sound);

    words = Notifier::batchNotification({.arrived = 1, .failed = 0, .track = u"Okuma Drift – Neon Rain"_s});
    EXPECT_EQ(words.body, u"“Okuma Drift – Neon Rain” is in your Inbox."_s);
    words = Notifier::batchNotification({.arrived = 1, .failed = 0, .track = {}});
    EXPECT_EQ(words.body, u"1 track is in your Inbox."_s);

    // Nothing arrived: the Get tracks list says why.
    words = Notifier::batchNotification({.arrived = 0, .failed = 2, .track = {}});
    EXPECT_EQ(words.body, u"2 tracks couldn't be downloaded."_s);
    EXPECT_EQ(words.target, (NotificationTarget{u"get"_s, {}}));
    words = Notifier::batchNotification({.arrived = 0, .failed = 1, .track = u"Okuma Drift – Neon Rain"_s});
    EXPECT_EQ(words.body, u"“Okuma Drift – Neon Rain” couldn't be downloaded."_s);

    words = Notifier::batchNotification({.arrived = 11, .failed = 3, .track = {}});
    EXPECT_EQ(words.body, u"11 tracks are in your Inbox; 3 couldn't be downloaded."_s);
    EXPECT_EQ(words.target, (NotificationTarget{u"inbox"_s, {}}));
}

TEST_F(NotifierTest, WhatComesFromTheNetworkIsBoundedAndOnOneLine)
{
    notifier.messageAdded(message(u"spam\nmer\t"_s, QString(1000, u'x')));
    ASSERT_EQ(sink.posted.size(), 1);
    const SystemNotification &posted = sink.posted.constFirst();
    EXPECT_EQ(posted.title, u"Message from spam mer"_s);
    EXPECT_EQ(posted.body.size(), Notifier::kMaxBody);
    EXPECT_TRUE(posted.body.endsWith(u'…'));
    EXPECT_EQ(posted.target.argument, u"spam\nmer\t"_s); // the conversation is still the peer's own name

    // A cut never splits a character in two.
    const QString emoji = u"🎧"_s; // two UTF-16 units
    QString text(Notifier::kMaxBody - 2, u'y');
    text += emoji + emoji;
    notifier.messageAdded(message(u"lena"_s, text));
    const QString body = sink.posted.constLast().body;
    EXPECT_LE(body.size(), Notifier::kMaxBody);
    EXPECT_FALSE(body.at(body.size() - 2).isHighSurrogate());
    EXPECT_TRUE(body.endsWith(u'…'));

    notifier.roomLineAdded(QString(500, u'r'),
                           {.atMs = 0, .user = u"lena"_s, .text = u"hi\u0007there"_s, .own = false, .mention = true});
    EXPECT_LE(sink.posted.constLast().title.size(), Notifier::kMaxTitle);
    EXPECT_EQ(sink.posted.constLast().body, u"hi there"_s);
}

TEST_F(NotifierTest, AClickOpensWhatTheNotificationWasAbout)
{
    const QSignalSpy activated(&notifier, &Notifier::activated);
    sink.click({u"messages"_s, u"vinyl_ghost"_s});
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.constFirst().at(0).value<NotificationTarget>(),
              (NotificationTarget{u"messages"_s, u"vinyl_ghost"_s}));
}

TEST(NotifierLifetimeTest, ASinkOutlivingItsNotifierHearsNoMore)
{
    selecta::testing::FakeNotificationSink sink;
    {
        Notifier notifier(&sink, [] { return true; });
        notifier.messageAdded(message(u"vinyl_ghost"_s, u"hi"_s));
    }
    sink.click({u"messages"_s, u"vinyl_ghost"_s}); // must not reach the notifier that's gone
    EXPECT_EQ(sink.posted.size(), 1);
}

TEST(NotifierLifetimeTest, WithoutASinkTheBadgeStillCounts)
{
    Notifier notifier(nullptr, [] { return true; });
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"hi"_s));
    notifier.getLineChanged(line(1, u"A"_s, u"One"_s, DownloadState::Queued));
    notifier.getLineChanged(line(1, u"A"_s, u"One"_s, DownloadState::Arrived));
    EXPECT_EQ(notifier.badge(), 1);
    EXPECT_EQ(notifier.sink(), nullptr);
}

TEST(RecordingNotificationSinkTest, KeepsTheLatestAndClicksThemById)
{
    selecta::app::RecordingNotificationSink sink;
    Notifier notifier(&sink, [] { return true; });
    QSignalSpy activated(&notifier, &Notifier::activated);
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"one"_s));
    notifier.messageAdded(message(u"vinyl_ghost"_s, u"two"_s)); // replaces the first, like Notification Center
    ASSERT_EQ(sink.posted().size(), 1);
    EXPECT_EQ(sink.posted().constFirst().body, u"two"_s);
    EXPECT_TRUE(sink.click(sink.posted().constFirst().id));
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.constFirst().at(0).value<NotificationTarget>().argument, u"vinyl_ghost"_s);
    EXPECT_FALSE(sink.click(u"no-such-id"_s));

    for (int i = 0; i < 2 * static_cast<int>(selecta::app::RecordingNotificationSink::kKept); ++i) {
        notifier.messageAdded(message(u"peer%1"_s.arg(i), u"hi"_s));
    }
    EXPECT_EQ(sink.posted().size(), static_cast<qsizetype>(selecta::app::RecordingNotificationSink::kKept));
    EXPECT_EQ(sink.posted().constLast().target.argument,
              u"peer%1"_s.arg(2 * selecta::app::RecordingNotificationSink::kKept - 1));
}

TEST(SystemNotificationSinkTest, OutsideAnAppBundleThereIsNone)
{
    // Tests run as plain executables: the system's notification centre would throw, so it is never touched.
    EXPECT_FALSE(selecta::app::runsInAppBundle());
    EXPECT_EQ(selecta::app::createSystemNotificationSink(), nullptr);
}
