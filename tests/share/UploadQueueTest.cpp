// SPDX-License-Identifier: GPL-3.0-or-later
// UploadQueue: slots, one upload per user, privileged users first, turns between users, FIFO per user, limits and
// their exact strings, repeats, removal and place in queue (sharing note §5.2, §5.3, §9.2).
#include "share/UploadQueue.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::share::UploadQueue;
namespace slsk = selecta::slsk;

namespace {

slsk::RemotePath file(const QString &name)
{
    return slsk::RemotePath::fromText(u"Music\\"_s + name);
}

UploadQueue::Options withSlots(int count)
{
    UploadQueue::Options options;
    options.uploadSlots = count;
    return options;
}

// Starts what next() gives and returns "user:name", or "" when nothing starts.
QString startNext(UploadQueue &queue)
{
    const std::optional<UploadQueue::Next> next = queue.next();
    if (!next) {
        return {};
    }
    EXPECT_TRUE(queue.started(next->user, next->path));
    return next->user + u':' + next->path.display().sliced(6);
}

// Serves everything with one slot, each upload ending before the next starts.
QStringList serveAll(UploadQueue &queue)
{
    QStringList order;
    while (const std::optional<UploadQueue::Next> next = queue.next()) {
        EXPECT_TRUE(queue.started(next->user, next->path));
        order.append(next->user + u':' + next->path.display().sliced(6));
        EXPECT_TRUE(queue.finished(next->user, next->path));
    }
    return order;
}

void enqueue(UploadQueue &queue, const QString &user, const QStringList &names, bool privileged = false)
{
    for (const QString &name : names) {
        EXPECT_TRUE(queue.enqueue(user, file(name), 1000, privileged).accepted);
    }
}

// --- Slots and one upload per user -----------------------------------------------------------------------------------

TEST(ShareUploadQueue, ServesEachUsersFilesInTheOrderQueued)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s, u"a3"_s});
    EXPECT_EQ(serveAll(queue), QStringList({u"alice:a1"_s, u"alice:a2"_s, u"alice:a3"_s}));
    EXPECT_EQ(queue.queued(), 0);
    EXPECT_EQ(queue.active(), 0);
}

TEST(ShareUploadQueue, NeverStartsTwoUploadsForOneUser)
{
    UploadQueue queue; // 3 slots
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_FALSE(queue.next().has_value()); // a slot is free, but alice is busy
    EXPECT_TRUE(queue.slotFree());
    EXPECT_TRUE(queue.finished(u"alice"_s, file(u"a1"_s)));
    EXPECT_EQ(startNext(queue), u"alice:a2"_s);
}

TEST(ShareUploadQueue, StartsNoMoreThanTheSlots)
{
    UploadQueue queue(withSlots(2));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s});
    enqueue(queue, u"carol"_s, {u"c1"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_EQ(startNext(queue), u"bob:b1"_s);
    EXPECT_FALSE(queue.slotFree());
    EXPECT_FALSE(queue.next().has_value());
    EXPECT_EQ(queue.active(), 2);
    EXPECT_EQ(queue.queued(), 1);
    EXPECT_TRUE(queue.finished(u"alice"_s, file(u"a1"_s)));
    EXPECT_EQ(startNext(queue), u"carol:c1"_s);
}

TEST(ShareUploadQueue, ZeroSlotsStartNothing)
{
    UploadQueue queue(withSlots(0));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    EXPECT_FALSE(queue.next().has_value());
    EXPECT_FALSE(queue.slotFree());
    queue.setOptions(withSlots(1));
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
}

TEST(ShareUploadQueue, NextChangesNothingUntilStarted)
{
    UploadQueue queue;
    enqueue(queue, u"alice"_s, {u"a1"_s});
    const std::optional<UploadQueue::Next> first = queue.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(queue.next(), first);
    EXPECT_EQ(first->user, u"alice"_s);
    EXPECT_EQ(first->path, file(u"a1"_s));
    EXPECT_EQ(first->size, 1000U);
    // It can't be started (the file isn't shared any more): removing it moves on.
    EXPECT_TRUE(queue.remove(u"alice"_s, file(u"a1"_s)));
    EXPECT_FALSE(queue.next().has_value());
}

// --- Turns -----------------------------------------------------------------------------------------------------------

TEST(ShareUploadQueue, UsersTakeTurns)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s, u"a3"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s, u"b2"_s});
    enqueue(queue, u"carol"_s, {u"c1"_s});
    EXPECT_EQ(serveAll(queue),
              QStringList({u"alice:a1"_s, u"bob:b1"_s, u"carol:c1"_s, u"alice:a2"_s, u"bob:b2"_s, u"alice:a3"_s}));
}

TEST(ShareUploadQueue, ANewcomerGoesBeforeAUserWhoseTurnEndedAfterTheyArrived)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    enqueue(queue, u"bob"_s, {u"b1"_s}); // arrives while alice's upload runs
    EXPECT_TRUE(queue.finished(u"alice"_s, file(u"a1"_s)));
    EXPECT_EQ(startNext(queue), u"bob:b1"_s);
}

TEST(ShareUploadQueue, ANewcomerWaitsBehindUsersAlreadyWaiting)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    enqueue(queue, u"carol"_s, {u"c1"_s});
    EXPECT_TRUE(queue.finished(u"alice"_s, file(u"a1"_s)));
    EXPECT_EQ(serveAll(queue), QStringList({u"bob:b1"_s, u"carol:c1"_s}));
}

TEST(ShareUploadQueue, QueueingMoreFilesDoesntCostATurn)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s});
    enqueue(queue, u"alice"_s, {u"a2"_s, u"a3"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
}

TEST(ShareUploadQueue, AReturningUserStartsAgainAtTheBack)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_TRUE(queue.finished(u"alice"_s, file(u"a1"_s))); // alice holds nothing now
    enqueue(queue, u"alice"_s, {u"a2"_s});                  // and comes straight back
    EXPECT_EQ(serveAll(queue), QStringList({u"bob:b1"_s, u"alice:a2"_s}));
}

// --- Privileged users ------------------------------------------------------------------------------------------------

TEST(ShareUploadQueue, PrivilegedUsersGoFirst)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s});
    enqueue(queue, u"vip"_s, {u"v1"_s, u"v2"_s}, true);
    enqueue(queue, u"bob"_s, {u"b1"_s});
    EXPECT_EQ(serveAll(queue), QStringList({u"vip:v1"_s, u"vip:v2"_s, u"alice:a1"_s, u"bob:b1"_s, u"alice:a2"_s}));
}

TEST(ShareUploadQueue, PrivilegedUsersTakeTurnsAmongThemselves)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"vip1"_s, {u"x1"_s, u"x2"_s}, true);
    enqueue(queue, u"vip2"_s, {u"y1"_s}, true);
    EXPECT_EQ(serveAll(queue), QStringList({u"vip1:x1"_s, u"vip2:y1"_s, u"vip1:x2"_s}));
}

TEST(ShareUploadQueue, ABusyPrivilegedUserDoesntHoldSlotsIdle)
{
    UploadQueue queue(withSlots(2));
    enqueue(queue, u"vip"_s, {u"v1"_s, u"v2"_s}, true);
    enqueue(queue, u"alice"_s, {u"a1"_s});
    EXPECT_EQ(startNext(queue), u"vip:v1"_s);
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
}

TEST(ShareUploadQueue, TheServersListMakesUsersPrivilegedAndReplacesWhatEnqueueSaid)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s}, true);
    queue.setPrivileged({u"carol"_s});
    enqueue(queue, u"carol"_s, {u"c1"_s}); // in the list, though enqueue doesn't say so
    EXPECT_EQ(serveAll(queue), QStringList({u"carol:c1"_s, u"alice:a1"_s, u"bob:b1"_s}));
}

TEST(ShareUploadQueue, TheLatestEnqueueSaysWhetherAUserIsPrivileged)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s});
    enqueue(queue, u"bob"_s, {u"b2"_s}, true);
    EXPECT_EQ(startNext(queue), u"bob:b1"_s);
}

// --- Limits ----------------------------------------------------------------------------------------------------------

TEST(ShareUploadQueue, RefusesBeyondTheFileLimitWithTheStandardString)
{
    UploadQueue::Options options;
    options.maxFilesPerUser = 2;
    UploadQueue queue(options);
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"1"_s), 10, false).accepted);
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"2"_s), 10, false).accepted);
    const UploadQueue::Decision refused = queue.enqueue(u"alice"_s, file(u"3"_s), 10, false);
    EXPECT_FALSE(refused.accepted);
    EXPECT_EQ(refused.reason, u"Too many files"_s);
    EXPECT_TRUE(queue.enqueue(u"bob"_s, file(u"3"_s), 10, false).accepted); // per user
    // Active files don't count: once one starts, another fits.
    EXPECT_TRUE(queue.started(u"alice"_s, file(u"1"_s)));
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"3"_s), 10, false).accepted);
}

TEST(ShareUploadQueue, RefusesBeyondTheByteLimitWithTheStandardString)
{
    UploadQueue::Options options;
    options.maxBytesPerUser = 1000;
    UploadQueue queue(options);
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"1"_s), 600, false).accepted);
    // Nicotine+'s rule: refused once the queued bytes reach the limit, so this one takes the queue past it.
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"2"_s), 600, false).accepted);
    const UploadQueue::Decision refused = queue.enqueue(u"alice"_s, file(u"3"_s), 1, false);
    EXPECT_FALSE(refused.accepted);
    EXPECT_EQ(refused.reason, u"Too many megabytes"_s);
    // A single file larger than the limit still fits into an empty queue.
    EXPECT_TRUE(queue.enqueue(u"bob"_s, file(u"huge"_s), 5000, false).accepted);
    // Starting a file frees its bytes.
    EXPECT_TRUE(queue.started(u"alice"_s, file(u"1"_s)));
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"3"_s), 1, false).accepted);
}

TEST(ShareUploadQueue, TheFileLimitIsCheckedBeforeTheByteLimit)
{
    UploadQueue::Options options;
    options.maxFilesPerUser = 1;
    options.maxBytesPerUser = 10;
    UploadQueue queue(options);
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"1"_s), 100, false).accepted);
    EXPECT_EQ(queue.enqueue(u"alice"_s, file(u"2"_s), 100, false).reason, u"Too many files"_s);
}

TEST(ShareUploadQueue, TheWholeQueueIsBounded)
{
    UploadQueue::Options options;
    options.maxFilesPerUser = UploadQueue::kMaxEntries;
    UploadQueue queue(options);
    for (int i = 0; i < UploadQueue::kMaxEntries; ++i) {
        ASSERT_TRUE(queue.enqueue(u"user%1"_s.arg(i % 50), file(QString::number(i)), 1, false).accepted);
    }
    EXPECT_EQ(queue.queued(), UploadQueue::kMaxEntries);
    const UploadQueue::Decision refused = queue.enqueue(u"newcomer"_s, file(u"x"_s), 1, false);
    EXPECT_FALSE(refused.accepted);
    EXPECT_EQ(refused.reason, u"Too many files"_s);
    // Active files count too: starting one frees nothing.
    EXPECT_TRUE(queue.started(u"user0"_s, file(u"0"_s)));
    EXPECT_FALSE(queue.enqueue(u"newcomer"_s, file(u"x"_s), 1, false).accepted);
    EXPECT_TRUE(queue.finished(u"user0"_s, file(u"0"_s)));
    EXPECT_TRUE(queue.enqueue(u"newcomer"_s, file(u"x"_s), 1, false).accepted);
}

TEST(ShareUploadQueue, OptionsAreClamped)
{
    UploadQueue::Options options;
    options.uploadSlots = -3;
    options.maxFilesPerUser = 1'000'000;
    options.maxBytesPerUser = -1;
    const UploadQueue queue(options);
    EXPECT_EQ(queue.options().uploadSlots, 0);
    EXPECT_EQ(queue.options().maxFilesPerUser, UploadQueue::kMaxEntries);
    EXPECT_EQ(queue.options().maxBytesPerUser, 0);

    UploadQueue none(UploadQueue::Options{1, 0, 1000});
    EXPECT_EQ(none.enqueue(u"alice"_s, file(u"1"_s), 1, false).reason, u"Too many files"_s);
    UploadQueue noBytes(UploadQueue::Options{1, 10, 0});
    EXPECT_EQ(noBytes.enqueue(u"alice"_s, file(u"1"_s), 1, false).reason, u"Too many megabytes"_s);
    EXPECT_EQ(UploadQueue().options(), UploadQueue::Options{});
    // Nicotine+'s defaults, through the governor: 3 slots, 100 files and 10 000 MiB per user.
    EXPECT_EQ(UploadQueue().options().uploadSlots, 3);
    EXPECT_EQ(UploadQueue().options().maxFilesPerUser, 100);
    EXPECT_EQ(UploadQueue().options().maxBytesPerUser, qint64{10'000} * 1024 * 1024);
}

// --- Repeats and removal ---------------------------------------------------------------------------------------------

TEST(ShareUploadQueue, ARepeatIsAcceptedWithoutASecondEntry)
{
    UploadQueue queue(withSlots(1));
    EXPECT_EQ(queue.enqueue(u"alice"_s, file(u"a1"_s), 10, false), (UploadQueue::Decision{true, false, {}}));
    EXPECT_EQ(queue.enqueue(u"alice"_s, file(u"a1"_s), 10, false), (UploadQueue::Decision{true, true, {}}));
    EXPECT_EQ(queue.queued(), 1);
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_EQ(queue.enqueue(u"alice"_s, file(u"a1"_s), 10, false), (UploadQueue::Decision{true, true, {}}));
    EXPECT_EQ(queue.queued(), 0);
    EXPECT_EQ(queue.active(), 1);
    // Another user asking for the same path is another entry.
    EXPECT_FALSE(queue.enqueue(u"bob"_s, file(u"a1"_s), 10, false).repeat);
}

TEST(ShareUploadQueue, ARepeatIsAcceptedEvenWhenTheLimitsAreReached)
{
    UploadQueue::Options options;
    options.maxFilesPerUser = 1;
    UploadQueue queue(options);
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"a1"_s), 10, false).accepted);
    EXPECT_TRUE(queue.enqueue(u"alice"_s, file(u"a1"_s), 10, false).repeat);
}

TEST(ShareUploadQueue, RemovesQueuedAndActiveFiles)
{
    UploadQueue queue(withSlots(2));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_TRUE(queue.remove(u"alice"_s, file(u"a2"_s)));
    EXPECT_EQ(queue.queued(), 0);
    EXPECT_TRUE(queue.remove(u"alice"_s, file(u"a1"_s)));
    EXPECT_EQ(queue.active(), 0);
    EXPECT_FALSE(queue.remove(u"alice"_s, file(u"a1"_s)));
    EXPECT_FALSE(queue.remove(u"nobody"_s, file(u"a1"_s)));
}

TEST(ShareUploadQueue, CallsAboutFilesItDoesntHoldChangeNothing)
{
    UploadQueue queue;
    enqueue(queue, u"alice"_s, {u"a1"_s});
    EXPECT_FALSE(queue.started(u"alice"_s, file(u"zz"_s)));
    EXPECT_FALSE(queue.started(u"bob"_s, file(u"a1"_s)));
    EXPECT_FALSE(queue.finished(u"alice"_s, file(u"a1"_s))); // queued, not active
    EXPECT_FALSE(queue.finished(u"bob"_s, file(u"a1"_s)));
    EXPECT_TRUE(queue.removeUser(u"bob"_s).isEmpty());
    EXPECT_EQ(queue.queued(), 1);
    EXPECT_EQ(queue.active(), 0);
    EXPECT_TRUE(queue.started(u"alice"_s, file(u"a1"_s)));
    EXPECT_FALSE(queue.started(u"alice"_s, file(u"a1"_s))); // already active
}

TEST(ShareUploadQueue, RemovingAUserReturnsTheirQueuedFiles)
{
    UploadQueue queue;
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s, u"a3"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_EQ(queue.removeUser(u"alice"_s), QList<slsk::RemotePath>({file(u"a2"_s), file(u"a3"_s)}));
    EXPECT_EQ(queue.queued(), 1);
    EXPECT_EQ(queue.active(), 0);
    EXPECT_FALSE(queue.finished(u"alice"_s, file(u"a1"_s)));
    EXPECT_EQ(startNext(queue), u"bob:b1"_s);
}

TEST(ShareUploadQueue, PathsAreComparedByTheirBytes)
{
    UploadQueue queue;
    EXPECT_TRUE(queue.enqueue(u"alice"_s, slsk::RemotePath{QByteArray("Music\\Caf\xe9.mp3")}, 1, false).accepted);
    EXPECT_FALSE(queue.enqueue(u"alice"_s, slsk::RemotePath::fromText(u"Music\\Caf\u00e9.mp3"_s), 1, false).repeat);
    EXPECT_EQ(queue.queued(), 2);
}

// --- Place in queue --------------------------------------------------------------------------------------------------

TEST(ShareUploadQueue, PlaceInQueueFollowsTheTurns)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s, u"a3"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s, u"b2"_s});
    enqueue(queue, u"carol"_s, {u"c1"_s});
    // The order serveAll() would give: a1 b1 c1 a2 b2 a3.
    const QList<std::pair<QString, QString>> order{{u"alice"_s, u"a1"_s}, {u"bob"_s, u"b1"_s}, {u"carol"_s, u"c1"_s},
                                                   {u"alice"_s, u"a2"_s}, {u"bob"_s, u"b2"_s}, {u"alice"_s, u"a3"_s}};
    for (qsizetype i = 0; i < order.size(); ++i) {
        EXPECT_EQ(queue.placeInQueue(order.at(i).first, file(order.at(i).second)), static_cast<quint32>(i + 1))
            << order.at(i).second.toStdString();
    }
    EXPECT_FALSE(queue.placeInQueue(u"alice"_s, file(u"zz"_s)).has_value());
    EXPECT_FALSE(queue.placeInQueue(u"dave"_s, file(u"a1"_s)).has_value());
}

TEST(ShareUploadQueue, PlaceInQueueMatchesWhatIsServed)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s, u"a3"_s, u"a4"_s});
    enqueue(queue, u"vip"_s, {u"v1"_s, u"v2"_s}, true);
    enqueue(queue, u"bob"_s, {u"b1"_s});
    enqueue(queue, u"carol"_s, {u"c1"_s, u"c2"_s});
    // Every queued file's place, before anything is served, is where serveAll() serves it.
    QHash<QString, quint32> places;
    for (const QString &name : {u"a1"_s, u"a2"_s, u"a3"_s, u"a4"_s}) {
        places.insert(u"alice:"_s + name, *queue.placeInQueue(u"alice"_s, file(name)));
    }
    for (const QString &name : {u"v1"_s, u"v2"_s}) {
        places.insert(u"vip:"_s + name, *queue.placeInQueue(u"vip"_s, file(name)));
    }
    places.insert(u"bob:b1"_s, *queue.placeInQueue(u"bob"_s, file(u"b1"_s)));
    for (const QString &name : {u"c1"_s, u"c2"_s}) {
        places.insert(u"carol:"_s + name, *queue.placeInQueue(u"carol"_s, file(name)));
    }
    const QStringList served = serveAll(queue);
    ASSERT_EQ(served.size(), places.size());
    for (qsizetype i = 0; i < served.size(); ++i) {
        EXPECT_EQ(places.value(served.at(i)), static_cast<quint32>(i + 1)) << served.at(i).toStdString();
    }
}

TEST(ShareUploadQueue, ActiveFilesHaveNoPlaceAndTheirUsersComeLast)
{
    UploadQueue queue(withSlots(2));
    enqueue(queue, u"alice"_s, {u"a1"_s, u"a2"_s});
    enqueue(queue, u"bob"_s, {u"b1"_s, u"b2"_s});
    EXPECT_EQ(startNext(queue), u"alice:a1"_s);
    EXPECT_FALSE(queue.placeInQueue(u"alice"_s, file(u"a1"_s)).has_value());
    // bob can start now; alice's next file comes after his.
    EXPECT_EQ(queue.placeInQueue(u"bob"_s, file(u"b1"_s)), 1U);
    EXPECT_EQ(queue.placeInQueue(u"alice"_s, file(u"a2"_s)), 2U);
    EXPECT_EQ(queue.placeInQueue(u"bob"_s, file(u"b2"_s)), 3U);
}

TEST(ShareUploadQueue, PrivilegedFilesComeFirstInPlaces)
{
    UploadQueue queue(withSlots(1));
    enqueue(queue, u"alice"_s, {u"a1"_s});
    enqueue(queue, u"vip"_s, {u"v1"_s, u"v2"_s, u"v3"_s}, true);
    EXPECT_EQ(queue.placeInQueue(u"vip"_s, file(u"v1"_s)), 1U);
    EXPECT_EQ(queue.placeInQueue(u"vip"_s, file(u"v3"_s)), 3U);
    EXPECT_EQ(queue.placeInQueue(u"alice"_s, file(u"a1"_s)), 4U);
}

} // namespace
