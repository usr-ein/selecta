// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/WishlistService.h"

#include "base/Clock.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::WishlistItem;
using selecta::core::WishlistService;

namespace {

struct FakeNetwork final : selecta::core::SearchNetwork
{
    QStringList plain;
    QStringList wishlist;
    quint32 nextToken = 100;

    bool isOnline() const override { return true; }
    quint32 search(const QString &query,
                   selecta::governor::Permit permit) override // clazy:exclude=function-args-by-ref
    {
        Q_UNUSED(permit)
        plain.append(query);
        return nextToken++;
    }
    quint32 searchWishlist(const QString &query,
                           selecta::governor::Permit permit) override // clazy:exclude=function-args-by-ref
    {
        Q_UNUSED(permit)
        wishlist.append(query);
        return nextToken++;
    }
    void stopSearch(quint32 token) override { Q_UNUSED(token) }
};

selecta::slsk::peer::FileSearchResponse answer(quint32 token, const QStringList &paths)
{
    selecta::slsk::peer::FileSearchResponse response;
    response.token = token;
    for (const QString &path : paths) {
        selecta::slsk::FileEntry file;
        file.name = selecta::slsk::RemotePath::fromText(path);
        file.size = 10'000'000;
        response.results.append(file);
    }
    response.slotFree = true;
    return response;
}

class WishlistServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        wishlist = std::make_unique<WishlistService>(db, search, [this] { return now; });
    }

    // Runs the next search and answers it with these files from vinyl_ghost.
    void runAndAnswer(const QStringList &paths)
    {
        const quint32 token = network.nextToken;
        wishlist->runNext();
        ASSERT_TRUE(QTest::qWaitFor([&] { return network.nextToken > token; }, 3000));
        search.received(token, u"vinyl_ghost"_s, answer(token, paths));
        clock.advance(11min); // past the governor's quiet time for the same search
        now += 660'000;
    }

    qint64 now = 1'800'000'000'000;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    FakeNetwork network;
    selecta::core::SearchService search{governor, network, clock, {.idle = 50ms, .total = 500ms}};
    selecta::storage::Database db;
    std::unique_ptr<WishlistService> wishlist;
};

} // namespace

TEST_F(WishlistServiceTest, SavesEachSearchOnce)
{
    const std::optional<qint64> first = wishlist->add(u"Okuma Drift  Neon Rain"_s);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(wishlist->add(u"neon rain okuma drift"_s), first); // the same words
    EXPECT_FALSE(wishlist->add(u"  "_s).has_value());
    const QList<WishlistItem> items = wishlist->items();
    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items.constFirst().text, u"Okuma Drift Neon Rain"_s);
    EXPECT_EQ(items.constFirst().lastRunMs, 0);
    wishlist->remove(*first);
    EXPECT_TRUE(wishlist->items().isEmpty());
}

TEST_F(WishlistServiceTest, ARunCountsTheFilesItHadntSeen)
{
    const std::optional<qint64> id = wishlist->add(u"Okuma Drift Neon Rain"_s);
    ASSERT_TRUE(id.has_value());
    QSignalSpy found(wishlist.get(), &WishlistService::found);

    // The first run: everything is new (audio only).
    runAndAnswer(
        {u"Music\\Okuma Drift - Neon Rain.flac"_s, u"Music\\Okuma Drift - Neon Rain.mp3"_s, u"Music\\cover.jpg"_s});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !found.isEmpty(); }, 3000));
    EXPECT_EQ(network.wishlist, QStringList{u"Okuma Drift Neon Rain"_s}); // the server's wishlist message
    EXPECT_TRUE(network.plain.isEmpty());
    EXPECT_EQ(found.at(0).at(1).toInt(), 2);
    EXPECT_EQ(wishlist->items().constFirst().found, 2);
    EXPECT_GT(wishlist->items().constFirst().lastRunMs, 0);

    // The same files again: nothing new; one more: one new.
    runAndAnswer({u"Music\\Okuma Drift - Neon Rain.flac"_s});
    ASSERT_TRUE(QTest::qWaitFor([&] { return network.wishlist.size() == 2; }, 3000));
    QTest::qWait(700); // the run ends
    EXPECT_EQ(found.size(), 1);
    runAndAnswer({u"Music\\Okuma Drift - Neon Rain (Extended Mix).flac"_s});
    ASSERT_TRUE(QTest::qWaitFor([&] { return found.size() == 2; }, 3000));
    EXPECT_EQ(found.at(1).at(1).toInt(), 1);
    EXPECT_EQ(wishlist->items().constFirst().found, 3);

    // Looked at: nothing new until the next find.
    wishlist->markSeen(*id);
    EXPECT_EQ(wishlist->items().constFirst().found, 0);
}

TEST_F(WishlistServiceTest, RunsTheSearchThatWaitedLongest)
{
    ASSERT_TRUE(wishlist->add(u"Okuma Drift Neon Rain"_s).has_value());
    now += 1000;
    ASSERT_TRUE(wishlist->add(u"Kassia Moreau Night Transit"_s).has_value());
    runAndAnswer({});
    QTest::qWait(700);
    runAndAnswer({});
    ASSERT_TRUE(QTest::qWaitFor([&] { return network.wishlist.size() == 2; }, 3000));
    EXPECT_EQ(network.wishlist,
              (QStringList{u"Okuma Drift Neon Rain"_s, u"Kassia Moreau Night Transit"_s})); // never run, oldest first
}
