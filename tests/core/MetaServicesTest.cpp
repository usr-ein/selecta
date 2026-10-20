// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MetaServices.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "governor/Governor.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::MetaServices;
using selecta::core::SpotifyStatus;
using State = selecta::meta::SpotifyAuth::State;

namespace {

const QString kClientId = u"0123456789abcdef0123456789abcdef"_s; // shaped like a Spotify Client ID; not a real one

class MetaServicesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        start(std::make_unique<selecta::meta::InMemoryTokenStore>());
    }

    // `allowed`: the user accepted the notice on Spotify's terms (D30).
    void start(std::unique_ptr<selecta::meta::InMemoryTokenStore> store, bool allowed = true)
    {
        MetaServices::Options options;
        options.spotifyAllowed = allowed;
        options.tokens = std::move(store);
        options.openUrl = [this](const QUrl &url) {
            opened = url;
            ++opens;
            return openable;
        };
        // Nothing listens on port 1: any request fails at once, and nothing ever reaches Spotify.
        options.endpoints = selecta::core::MetaEndpoints::unreachable();
        services = std::make_unique<MetaServices>(governor, log, clock, std::move(options));
        tokens = dynamic_cast<selecta::meta::InMemoryTokenStore *>(&services->tokens()); // the store given above
        ASSERT_NE(tokens, nullptr);
    }

    // Lets futures and queued calls run until `done` holds or a second has passed.
    template <typename Condition>
    bool settle(Condition done)
    {
        return QTest::qWaitFor(done, 1000);
    }

    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore *tokens = nullptr;
    std::unique_ptr<MetaServices> services;
    QUrl opened;
    int opens = 0;
    bool openable = true; // the browser opens
};

} // namespace

TEST_F(MetaServicesTest, NothingSavedMeansNotConfigured)
{
    services->restore();
    QTest::qWait(20);
    EXPECT_EQ(services->spotifyStatus().state, State::NotConfigured);
    EXPECT_FALSE(services->spotifyStatus().hasSecret);
}

TEST_F(MetaServicesTest, AClientIdIsSavedAndLeavesUsSignedOut)
{
    services->setSpotifyCredentials(kClientId, {}, false);
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().state == State::SignedOut; }));
    EXPECT_EQ(services->spotifyStatus().clientId, kClientId);
    ASSERT_TRUE(settle([this] { return tokens->peek(QString(MetaServices::kClientIdAccount)).has_value(); }));
    EXPECT_EQ(*tokens->peek(QString(MetaServices::kClientIdAccount)), kClientId.toUtf8());
}

TEST_F(MetaServicesTest, ASecretIsKeptInTheStoreAndNeverShown)
{
    QSignalSpy statuses(services.get(), &MetaServices::spotifyStatusChanged);
    services->setSpotifyCredentials(kClientId, "not-a-real-secret", false);
    ASSERT_TRUE(settle([this] { return tokens->peek(QString(MetaServices::kClientSecretAccount)).has_value(); }));
    EXPECT_TRUE(services->spotifyStatus().hasSecret);
    // The client-credentials request can't reach anything here, so it fails and says so.
    ASSERT_TRUE(settle([this] { return !services->spotifyStatus().error.isEmpty(); }));
    for (const auto &emitted : std::as_const(statuses)) {
        const auto status = emitted.at(0).value<SpotifyStatus>();
        EXPECT_FALSE(status.error.contains(u"not-a-real-secret"_s));
    }
}

TEST_F(MetaServicesTest, KeepingTheSavedSecret)
{
    services->setSpotifyCredentials(kClientId, "first-secret", false);
    services->setSpotifyCredentials(kClientId, {}, true);
    ASSERT_TRUE(settle([this] { return tokens->peek(QString(MetaServices::kClientSecretAccount)).has_value(); }));
    EXPECT_TRUE(services->spotifyStatus().hasSecret);
    services->setSpotifyCredentials(kClientId, {}, false); // now delete it
    ASSERT_TRUE(settle([this] { return !tokens->peek(QString(MetaServices::kClientSecretAccount)).has_value(); }));
    EXPECT_FALSE(services->spotifyStatus().hasSecret);
}

TEST_F(MetaServicesTest, AnEmptyClientIdForgetsEverything)
{
    services->setSpotifyCredentials(kClientId, "secret", false);
    ASSERT_TRUE(settle([this] { return tokens->peek(QString(MetaServices::kClientSecretAccount)).has_value(); }));
    services->setSpotifyCredentials(QString(), {}, false);
    ASSERT_TRUE(settle([this] {
        return !tokens->peek(QString(MetaServices::kClientIdAccount)) &&
               !tokens->peek(QString(MetaServices::kClientSecretAccount));
    }));
    EXPECT_EQ(services->spotifyStatus().state, State::NotConfigured);
    EXPECT_FALSE(services->spotifyStatus().hasSecret);
}

TEST_F(MetaServicesTest, RestoreReadsWhatWasSaved)
{
    services->setSpotifyCredentials(kClientId, {}, false);
    ASSERT_TRUE(settle([this] { return tokens->peek(QString(MetaServices::kClientIdAccount)).has_value(); }));

    // A new session over a store that already holds the Client ID.
    auto store = std::make_unique<selecta::meta::InMemoryTokenStore>();
    std::ignore = store->write(QString(MetaServices::kClientIdAccount), kClientId.toUtf8());
    QTest::qWait(10);
    services.reset();
    start(std::move(store));
    services->restore();
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().clientId == kClientId; }));
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().state == State::SignedOut; }));
}

TEST_F(MetaServicesTest, UntilTheTermsAreAcceptedSpotifyIsNeverSignedInTo)
{
    // Credentials saved before the user answered the notice (D30): read and shown, never used.
    auto store = std::make_unique<selecta::meta::InMemoryTokenStore>();
    std::ignore = store->write(QString(MetaServices::kClientIdAccount), kClientId.toUtf8());
    std::ignore = store->write(QString(MetaServices::kClientSecretAccount), "not-a-real-secret");
    QTest::qWait(10);
    services.reset();
    start(std::move(store), false);
    EXPECT_FALSE(services->isSpotifyAllowed());
    services->restore();
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().clientId == kClientId; }));
    EXPECT_EQ(services->spotifyStatus().state, State::SignedOut);
    EXPECT_TRUE(services->spotifyStatus().hasSecret);
    // A client-credentials sign-in would fail here (nothing listens) and say so: no word means none was tried.
    QTest::qWait(100);
    EXPECT_TRUE(services->spotifyStatus().error.isEmpty()) << services->spotifyStatus().error.toStdString();
    EXPECT_FALSE(services->spotify().isAvailable());

    // New credentials are saved, still without a sign-in; the browser sign-in is refused, and says why.
    services->setSpotifyCredentials(kClientId, "another-secret", false);
    QTest::qWait(100);
    EXPECT_TRUE(services->spotifyStatus().error.isEmpty());
    services->signInToSpotify();
    EXPECT_FALSE(opened.isValid());
    EXPECT_FALSE(services->spotifyStatus().error.isEmpty());
    EXPECT_NE(services->spotifyStatus().state, State::Authorizing);

    // Accepted: the next step signs in.
    services->allowSpotify();
    EXPECT_TRUE(services->isSpotifyAllowed());
    services->signInToSpotify();
    ASSERT_TRUE(settle([this] { return opened.isValid(); }));
    EXPECT_EQ(opened.host(), u"127.0.0.1"_s); // the fake accounts service, never Spotify
    EXPECT_EQ(services->spotifyStatus().state, State::Authorizing);
}

TEST_F(MetaServicesTest, SignInOpensTheBrowser)
{
    services->setSpotifyCredentials(kClientId, {}, false);
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().state == State::SignedOut; }));
    services->signInToSpotify();
    ASSERT_TRUE(settle([this] { return opened.isValid(); }));
    EXPECT_EQ(opened.host(), u"127.0.0.1"_s); // the fake accounts service, never Spotify
    EXPECT_EQ(services->spotifyStatus().state, State::Authorizing);
}

TEST_F(MetaServicesTest, TheSignInPageOpensAgainWhileTheSignInWaits)
{
    services->reopenSpotifySignIn(); // no sign-in: nothing to open
    EXPECT_EQ(opens, 0);
    services->setSpotifyCredentials(kClientId, {}, false);
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().state == State::SignedOut; }));
    services->signInToSpotify();
    ASSERT_TRUE(settle([this] { return opens == 1; }));
    const QUrl page = opened;
    ASSERT_EQ(services->spotifyStatus().state, State::Authorizing);

    // The same page, the same sign-in: its state and the listener it answers stay.
    services->reopenSpotifySignIn();
    EXPECT_EQ(opens, 2);
    EXPECT_EQ(opened, page);
    EXPECT_EQ(services->spotifyStatus().state, State::Authorizing);

    // A browser that doesn't open says so, and the sign-in still waits.
    openable = false;
    services->reopenSpotifySignIn();
    EXPECT_EQ(opens, 3);
    EXPECT_EQ(services->spotifyStatus().error, u"Selecta couldn't open your browser."_s);
    EXPECT_EQ(services->spotifyStatus().state, State::Authorizing);

    // Once the sign-in is over, there's no page to open.
    openable = true;
    services->spotifyAuth().cancelSignIn();
    ASSERT_TRUE(settle([this] { return services->spotifyStatus().state == State::SignedOut; }));
    services->reopenSpotifySignIn();
    EXPECT_EQ(opens, 3);
}
