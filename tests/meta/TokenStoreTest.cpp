// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/TokenStore.h"

#include "meta/KeychainTokenStore.h"
#include "meta/support/Await.h"

#include <QUuid>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::meta;
using selecta::meta::testing::await;

TEST(InMemoryTokenStore, MissingAccountReadsAsNone)
{
    InMemoryTokenStore store;
    const auto result = await(store.read(u"spotify/abc"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok());
    EXPECT_FALSE(result->value().has_value());
}

TEST(InMemoryTokenStore, WritesReadsReplacesAndRemoves)
{
    InMemoryTokenStore store;
    ASSERT_TRUE(await(store.write(u"a"_s, "one"))->ok());
    EXPECT_EQ(await(store.read(u"a"_s))->value().value(), "one");
    ASSERT_TRUE(await(store.write(u"a"_s, "two"))->ok());
    EXPECT_EQ(await(store.read(u"a"_s))->value().value(), "two");
    ASSERT_TRUE(await(store.remove(u"a"_s))->ok());
    EXPECT_FALSE(await(store.read(u"a"_s))->value().has_value());
    // Removing what isn't there succeeds.
    EXPECT_TRUE(await(store.remove(u"a"_s))->ok());
}

TEST(InMemoryTokenStore, NeverFinishesInsideTheCall)
{
    InMemoryTokenStore store;
    const QFuture<Result<void>> future = store.write(u"a"_s, "x");
    EXPECT_FALSE(future.isFinished());
    EXPECT_TRUE(await(future).has_value());
}

TEST(InMemoryTokenStore, FailingModeFailsEveryCall)
{
    InMemoryTokenStore store;
    store.setFailing(true);
    EXPECT_FALSE(await(store.read(u"a"_s))->ok());
    EXPECT_FALSE(await(store.write(u"a"_s, "x"))->ok());
    EXPECT_FALSE(await(store.remove(u"a"_s))->ok());
    EXPECT_EQ(await(store.write(u"a"_s, "x"))->error().service, Service::Keychain);
}

// The real Keychain is touched only on request (it may show a system prompt): SELECTA_TEST_KEYCHAIN=1.
TEST(KeychainTokenStore, RoundTripInTheLoginKeychainOnRequest)
{
    if (qEnvironmentVariableIntValue("SELECTA_TEST_KEYCHAIN") != 1) {
        GTEST_SKIP() << "set SELECTA_TEST_KEYCHAIN=1 to exercise the real Keychain";
    }
    KeychainTokenStore store(u"app.selecta.tests."_s + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString account = u"spotify/test"_s;
    ASSERT_TRUE(await(store.remove(account))->ok());
    EXPECT_FALSE(await(store.read(account))->value().has_value());
    ASSERT_TRUE(await(store.write(account, "secret-1"))->ok());
    EXPECT_EQ(await(store.read(account))->value().value(), "secret-1");
    ASSERT_TRUE(await(store.write(account, "secret-2"))->ok());
    EXPECT_EQ(await(store.read(account))->value().value(), "secret-2");
    ASSERT_TRUE(await(store.remove(account))->ok());
    EXPECT_FALSE(await(store.read(account))->value().has_value());
}

TEST(KeychainTokenStore, DestroyingWithCallsQueuedDoesNotHang)
{
    if (qEnvironmentVariableIntValue("SELECTA_TEST_KEYCHAIN") != 1) {
        GTEST_SKIP() << "set SELECTA_TEST_KEYCHAIN=1 to exercise the real Keychain";
    }
    QFuture<Result<std::optional<QByteArray>>> last;
    {
        KeychainTokenStore store(u"app.selecta.tests."_s + QUuid::createUuid().toString(QUuid::WithoutBraces));
        for (int i = 0; i < 20; ++i) {
            last = store.read(u"missing"_s);
        }
    }
    EXPECT_TRUE(last.isFinished()); // either answered or cancelled, never left pending
}
