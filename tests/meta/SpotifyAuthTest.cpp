// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/SpotifyAuth.h"

#include "meta/Pkce.h"
#include "meta/TokenStore.h"
#include "meta/detail/ServiceGate.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
using selecta::base::ManualClock;
using selecta::meta::testing::await;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::spinUntil;
using State = SpotifyAuth::State;

namespace {

const QString kClientId = u"0123456789abcdef0123456789abcdef"_s;
const QString kTokenUrl = u"^https://accounts\\.test/api/token$"_s;

// What the "browser" does with the authorisation page.
enum class BrowserAction : quint8 {
    Approve,
    Deny,
    WrongState,
    Nothing, // the user never comes back
};

QByteArray tokenJson(const QByteArray &access, const QByteArray &refresh, int expiresIn = 3600,
                     const QString &scope = u"playlist-read-private"_s)
{
    QJsonObject o;
    o.insert("access_token"_L1, QString::fromLatin1(access));
    o.insert("token_type"_L1, u"Bearer"_s);
    o.insert("expires_in"_L1, expiresIn);
    if (!refresh.isEmpty()) {
        o.insert("refresh_token"_L1, QString::fromLatin1(refresh));
    }
    o.insert("scope"_L1, scope);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray storedRecord(const QString &clientId, const QByteArray &refresh, const QDateTime &authorisedAt)
{
    QJsonObject o;
    o.insert("v"_L1, 1);
    o.insert("client_id"_L1, clientId);
    o.insert("refresh_token"_L1, QString::fromLatin1(refresh));
    o.insert("authorised_at"_L1, authorisedAt.toString(Qt::ISODate));
    o.insert("scopes"_L1, QJsonArray{u"user-library-read"_s});
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QUrlQuery formOf(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}

class SpotifyAuthTest : public ::testing::Test
{
protected:
    void SetUp() override { makeAuth(kClientId); }

    void makeAuth(const QString &clientId, quint16 port = 0, std::chrono::milliseconds callbackTimeout = 5000ms)
    {
        SpotifyAuth::Config config;
        config.clientId = clientId;
        config.callbackPort = port;
        config.callbackTimeout = callbackTimeout;
        config.accountsUrl = QUrl(u"https://accounts.test"_s);
        config.openUrl = [this](const QUrl &url) {
            opened.append(url);
            if (!browserWorks) {
                return false;
            }
            QTimer::singleShot(0, &network, [this, url] { browse(url); });
            return true;
        };
        auth = std::make_unique<SpotifyAuth>(config, &network, pacer, store, clock);
        states = std::make_unique<QSignalSpy>(auth.get(), &SpotifyAuth::stateChanged);
        signedOut = std::make_unique<QSignalSpy>(auth.get(), &SpotifyAuth::signedOut);
        persistFailures = std::make_unique<QSignalSpy>(auth.get(), &SpotifyAuth::persistFailed);
    }

    // The fake browser: follows the redirect back to Selecta's loopback listener.
    void browse(const QUrl &authorizeUrl)
    {
        const QUrlQuery query(authorizeUrl);
        const QUrl redirect(query.queryItemValue(u"redirect_uri"_s, QUrl::FullyDecoded));
        const QString state = query.queryItemValue(u"state"_s, QUrl::FullyDecoded);
        QByteArray target;
        switch (browserAction) {
        case BrowserAction::Approve:
            target = "/callback?code=" + browserCode + "&state=" + state.toLatin1();
            break;
        case BrowserAction::Deny:
            target = "/callback?error=access_denied&state=" + state.toLatin1();
            break;
        case BrowserAction::WrongState:
            target = "/callback?code=" + browserCode + "&state=forged";
            break;
        case BrowserAction::Nothing:
            return;
        }
        auto *socket = new QTcpSocket(auth.get());
        QObject::connect(socket, &QTcpSocket::connected, socket,
                         [socket, target] { socket->write("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        socket->connectToHost(QHostAddress(QHostAddress::LocalHost), static_cast<quint16>(redirect.port()));
    }

    // Signs in through the fake browser with a token answer queued.
    void signInOk(const QByteArray &access = "ACCESS1", const QByteArray &refresh = "REFRESH1")
    {
        network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson(access, refresh)));
        const auto result = await(auth->signIn({u"playlist-read-private"_s}));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    }

    // Lets the accounts gate's spacing pass before the next token request.
    void advance(std::chrono::milliseconds by)
    {
        clock.advance(by);
        selecta::meta::detail::ServiceGate::pollAll();
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    InMemoryTokenStore store;
    std::unique_ptr<SpotifyAuth> auth;
    std::unique_ptr<QSignalSpy> states;
    std::unique_ptr<QSignalSpy> signedOut;
    std::unique_ptr<QSignalSpy> persistFailures;
    QList<QUrl> opened;
    bool browserWorks = true;
    BrowserAction browserAction = BrowserAction::Approve;
    QByteArray browserCode = "AUTHCODE";
};

} // namespace

TEST_F(SpotifyAuthTest, StartsSignedOutWithAClientIdAndNotConfiguredWithout)
{
    EXPECT_EQ(auth->state(), State::SignedOut);
    makeAuth(QString());
    EXPECT_EQ(auth->state(), State::NotConfigured);
    const auto result = await(auth->signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotConfigured);
    EXPECT_TRUE(opened.isEmpty());
}

TEST_F(SpotifyAuthTest, SignInWithPkceEndToEnd)
{
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("ACCESS1", "REFRESH1")));
    const QFuture<Result<void>> future = auth->signIn({u"playlist-read-private"_s});
    EXPECT_FALSE(future.isFinished());
    EXPECT_EQ(auth->state(), State::Authorizing);
    const auto result = await(future);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(auth->state(), State::SignedIn);
    EXPECT_EQ(auth->mode(), SpotifyAuth::Mode::User);

    // The authorisation URL.
    ASSERT_EQ(opened.size(), 1);
    const QUrl url = opened.first();
    EXPECT_EQ(url.host(), u"accounts.test"_s);
    EXPECT_EQ(url.path(), u"/authorize"_s);
    const QUrlQuery query(url);
    EXPECT_EQ(query.queryItemValue(u"client_id"_s), kClientId);
    EXPECT_EQ(query.queryItemValue(u"response_type"_s), u"code"_s);
    EXPECT_EQ(query.queryItemValue(u"code_challenge_method"_s), u"S256"_s);
    EXPECT_EQ(query.queryItemValue(u"scope"_s, QUrl::FullyDecoded), u"playlist-read-private"_s);
    const QUrl redirect(query.queryItemValue(u"redirect_uri"_s, QUrl::FullyDecoded));
    EXPECT_EQ(redirect.scheme(), u"http"_s);
    EXPECT_EQ(redirect.host(), u"127.0.0.1"_s);
    EXPECT_GT(redirect.port(), 0);
    EXPECT_EQ(redirect.path(), u"/callback"_s);
    EXPECT_EQ(query.queryItemValue(u"state"_s).size(), 22);

    // The token exchange proves possession of the verifier.
    ASSERT_EQ(network.requestCount(), 1);
    const auto token = network.requests().first();
    EXPECT_EQ(token.method, "POST");
    EXPECT_EQ(token.header("Content-Type"), "application/x-www-form-urlencoded");
    EXPECT_TRUE(token.header("Authorization").isEmpty()); // PKCE: no secret
    const QUrlQuery form = formOf(token.body);
    EXPECT_EQ(form.queryItemValue(u"grant_type"_s), u"authorization_code"_s);
    EXPECT_EQ(form.queryItemValue(u"code"_s), u"AUTHCODE"_s);
    EXPECT_EQ(form.queryItemValue(u"client_id"_s), kClientId);
    EXPECT_EQ(QUrl(form.queryItemValue(u"redirect_uri"_s, QUrl::FullyDecoded)), redirect);
    const QByteArray verifier = form.queryItemValue(u"code_verifier"_s, QUrl::FullyDecoded).toLatin1();
    EXPECT_TRUE(pkce::isValidVerifier(verifier));
    EXPECT_EQ(QString::fromLatin1(pkce::challengeS256(verifier)), query.queryItemValue(u"code_challenge"_s));

    // Tokens: the access token in memory, the refresh token in the store.
    EXPECT_EQ(auth->grantedScopes(), QStringList{u"playlist-read-private"_s});
    EXPECT_TRUE(auth->hasScopes({u"playlist-read-private"_s}));
    EXPECT_FALSE(auth->hasScopes(SpotifyAuth::libraryScopes()));
    ASSERT_TRUE(spinUntil([&] { return store.peek(u"spotify/"_s + kClientId).has_value(); }));
    const QJsonObject record = QJsonDocument::fromJson(*store.peek(u"spotify/"_s + kClientId)).object();
    EXPECT_EQ(record.value("refresh_token"_L1).toString(), u"REFRESH1"_s);
    EXPECT_EQ(record.value("client_id"_L1).toString(), kClientId);
    ASSERT_TRUE(auth->signInExpiresAt().has_value());
    const QDateTime expires = *auth->signInExpiresAt();
    EXPECT_GE(expires, QDateTime::currentDateTimeUtc().addMonths(6).addSecs(-60));
    EXPECT_LE(expires, QDateTime::currentDateTimeUtc().addMonths(6).addSecs(60));

    const auto access = await(auth->accessToken());
    ASSERT_TRUE(access.has_value());
    EXPECT_EQ(access->value(), u"ACCESS1"_s);
    EXPECT_EQ(network.requestCount(), 1); // no refresh needed
}

TEST_F(SpotifyAuthTest, DeniedInTheBrowserIsAccessDenied)
{
    browserAction = BrowserAction::Deny;
    const auto result = await(auth->signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::AccessDenied);
    EXPECT_EQ(auth->state(), State::SignedOut);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(SpotifyAuthTest, ForgedStateIsRejected)
{
    browserAction = BrowserAction::WrongState;
    const auto result = await(auth->signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::StateMismatch);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(SpotifyAuthTest, NoAnswerInTimeIsATimeout)
{
    makeAuth(kClientId, 0, 150ms);
    browserAction = BrowserAction::Nothing;
    const auto result = await(auth->signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::Timeout);
    EXPECT_EQ(auth->state(), State::SignedOut);
}

TEST_F(SpotifyAuthTest, PortInUseIsReported)
{
    QTcpServer occupier;
    ASSERT_TRUE(occupier.listen(QHostAddress(QHostAddress::LocalHost), 0));
    makeAuth(kClientId, occupier.serverPort());
    const auto result = await(auth->signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::PortUnavailable);
    EXPECT_TRUE(opened.isEmpty());
}

TEST_F(SpotifyAuthTest, FixedPortIsUsedInTheRedirect)
{
    QTcpServer probe;
    ASSERT_TRUE(probe.listen(QHostAddress(QHostAddress::LocalHost), 0));
    const quint16 port = probe.serverPort();
    probe.close();
    makeAuth(kClientId, port);
    signInOk();
    const QUrl redirect(QUrlQuery(opened.first()).queryItemValue(u"redirect_uri"_s, QUrl::FullyDecoded));
    EXPECT_EQ(redirect.port(), port);
}

TEST_F(SpotifyAuthTest, BrowserThatCantOpenFails)
{
    browserWorks = false;
    const QFuture<Result<void>> future = auth->signIn();
    EXPECT_FALSE(future.isFinished());
    const auto result = await(future);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotConfigured);
    EXPECT_EQ(auth->state(), State::SignedOut);
}

TEST_F(SpotifyAuthTest, MissingOpenUrlIsNotConfigured)
{
    SpotifyAuth::Config config;
    config.clientId = kClientId;
    SpotifyAuth bare(config, &network, pacer, store, clock);
    const auto result = await(bare.signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotConfigured);
}

TEST_F(SpotifyAuthTest, TokenEndpointErrorsMapCleanly)
{
    network.enqueue("POST", kTokenUrl, FakeResponse::json(R"({"error":"invalid_grant"})", 400));
    auto result = await(auth->signIn());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);

    advance(10s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(R"({"error":"invalid_client"})", 400));
    result = await(auth->signIn());
    EXPECT_EQ(result->error().code, ErrorCode::NotConfigured);

    advance(10s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("A", ""))); // no refresh token
    result = await(auth->signIn());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);

    advance(10s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json("{not json"));
    result = await(auth->signIn());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);

    advance(10s);
    QJsonObject mac = QJsonDocument::fromJson(tokenJson("A", "R")).object();
    mac.insert("token_type"_L1, u"mac"_s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(QJsonDocument(mac).toJson()));
    result = await(auth->signIn());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);
    EXPECT_EQ(auth->state(), State::SignedOut);
}

TEST_F(SpotifyAuthTest, RefreshesWhenTheAccessTokenNearsExpiry)
{
    signInOk();
    advance(3600s - SpotifyAuth::kRefreshMargin + 1s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("ACCESS2", "")));
    const auto access = await(auth->accessToken());
    ASSERT_TRUE(access.has_value());
    ASSERT_TRUE(access->ok()) << access->error().toString().toStdString();
    EXPECT_EQ(access->value(), u"ACCESS2"_s);
    ASSERT_EQ(network.requestCount(), 2);
    const QUrlQuery form = formOf(network.requests().at(1).body);
    EXPECT_EQ(form.queryItemValue(u"grant_type"_s), u"refresh_token"_s);
    EXPECT_EQ(form.queryItemValue(u"refresh_token"_s), u"REFRESH1"_s);
    EXPECT_EQ(form.queryItemValue(u"client_id"_s), kClientId);
    // No rotation: the old refresh token stays.
    QTest::qWait(20);
    EXPECT_EQ(
        QJsonDocument::fromJson(*store.peek(u"spotify/"_s + kClientId)).object().value("refresh_token"_L1).toString(),
        u"REFRESH1"_s);
}

TEST_F(SpotifyAuthTest, RotatedRefreshTokenIsStored)
{
    signInOk();
    advance(3600s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("ACCESS2", "REFRESH2")));
    ASSERT_TRUE(await(auth->accessToken())->ok());
    ASSERT_TRUE(spinUntil([&] {
        return QJsonDocument::fromJson(store.peek(u"spotify/"_s + kClientId).value_or(QByteArray()))
                   .object()
                   .value("refresh_token"_L1)
                   .toString() == u"REFRESH2"_s;
    }));
}

TEST_F(SpotifyAuthTest, ConcurrentCallersShareOneRefresh)
{
    signInOk();
    advance(3600s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("ACCESS2", "")));
    const auto a = auth->accessToken();
    const auto b = auth->accessToken();
    const auto c = auth->accessToken();
    EXPECT_EQ(await(a)->value(), u"ACCESS2"_s);
    EXPECT_EQ(await(b)->value(), u"ACCESS2"_s);
    EXPECT_EQ(await(c)->value(), u"ACCESS2"_s);
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(SpotifyAuthTest, ExpiredRefreshTokenEndsTheSignIn)
{
    signInOk();
    advance(3600s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(R"({"error":"invalid_grant"})", 400));
    const auto access = await(auth->accessToken());
    ASSERT_TRUE(access.has_value());
    EXPECT_EQ(access->error().code, ErrorCode::SignInRequired);
    EXPECT_EQ(auth->state(), State::Expired);
    EXPECT_EQ(signedOut->size(), 1);
    EXPECT_FALSE(auth->signInExpiresAt().has_value());
    ASSERT_TRUE(spinUntil([&] { return !store.peek(u"spotify/"_s + kClientId).has_value(); }));
    // Later calls fail at once without a request.
    const int before = network.requestCount();
    EXPECT_EQ(await(auth->accessToken())->error().code, ErrorCode::SignInRequired);
    EXPECT_EQ(network.requestCount(), before);
}

TEST_F(SpotifyAuthTest, TransientRefreshFailureKeepsTheSignIn)
{
    signInOk();
    advance(3600s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json("{}", 400)); // no error code: not invalid_grant
    auto access = await(auth->accessToken());
    ASSERT_TRUE(access.has_value());
    EXPECT_FALSE(access->ok());
    EXPECT_EQ(auth->state(), State::SignedIn);

    advance(10s);
    FakeResponse refused;
    refused.networkError = QNetworkReply::HostNotFoundError;
    network.enqueue("POST", kTokenUrl, refused);
    access = await(auth->accessToken());
    EXPECT_EQ(access->error().code, ErrorCode::Network);
    EXPECT_EQ(auth->state(), State::SignedIn);

    advance(10s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("ACCESS3", "")));
    access = await(auth->accessToken());
    EXPECT_EQ(access->value(), u"ACCESS3"_s);
}

TEST_F(SpotifyAuthTest, UnauthorizedTokenIsRefreshedNextTime)
{
    signInOk();
    auth->reportUnauthorized(u"SOMETHING-ELSE"_s); // not the current token: ignored
    EXPECT_EQ(await(auth->accessToken())->value(), u"ACCESS1"_s);
    advance(10s);
    auth->reportUnauthorized(u"ACCESS1"_s);
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("ACCESS2", "")));
    EXPECT_EQ(await(auth->accessToken())->value(), u"ACCESS2"_s);
}

TEST_F(SpotifyAuthTest, RestoresAStoredSignIn)
{
    ASSERT_TRUE(await(store.write(u"spotify/"_s + kClientId,
                                  storedRecord(kClientId, "STORED", QDateTime::currentDateTimeUtc().addDays(-10))))
                    ->ok());
    const QFuture<Result<void>> future = auth->restore();
    EXPECT_EQ(auth->state(), State::Restoring);
    const auto result = await(future);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(auth->state(), State::SignedIn);
    EXPECT_TRUE(auth->hasScopes(SpotifyAuth::libraryScopes()));
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("FRESH", "")));
    EXPECT_EQ(await(auth->accessToken())->value(), u"FRESH"_s);
    EXPECT_EQ(formOf(network.requests().first().body).queryItemValue(u"refresh_token"_s), u"STORED"_s);
}

TEST_F(SpotifyAuthTest, RestoreWithNothingStoredStaysSignedOut)
{
    const auto result = await(auth->restore());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok());
    EXPECT_EQ(auth->state(), State::SignedOut);
}

TEST_F(SpotifyAuthTest, RestoreDropsACorruptRecord)
{
    ASSERT_TRUE(await(store.write(u"spotify/"_s + kClientId, "garbage"))->ok());
    const auto result = await(auth->restore());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(auth->state(), State::SignedOut);
    ASSERT_TRUE(spinUntil([&] { return !store.peek(u"spotify/"_s + kClientId).has_value(); }));
}

TEST_F(SpotifyAuthTest, RestoreOfASixMonthOldSignInIsExpired)
{
    ASSERT_TRUE(await(store.write(u"spotify/"_s + kClientId,
                                  storedRecord(kClientId, "OLD", QDateTime::currentDateTimeUtc().addMonths(-7))))
                    ->ok());
    const auto result = await(auth->restore());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(auth->state(), State::Expired);
    EXPECT_EQ(signedOut->size(), 1);
    ASSERT_TRUE(spinUntil([&] { return !store.peek(u"spotify/"_s + kClientId).has_value(); }));
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(SpotifyAuthTest, RestoreReportsAStoreFailure)
{
    store.setFailing(true);
    const auto result = await(auth->restore());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok());
    EXPECT_EQ(result->error().service, Service::Keychain);
    EXPECT_EQ(auth->state(), State::SignedOut);
}

TEST_F(SpotifyAuthTest, PersistFailureIsSignalledButTheSignInWorks)
{
    store.setFailing(true);
    signInOk();
    EXPECT_EQ(auth->state(), State::SignedIn);
    ASSERT_TRUE(spinUntil([&] { return persistFailures->size() == 1; }));
}

TEST_F(SpotifyAuthTest, SignOutForgetsEverything)
{
    signInOk();
    ASSERT_TRUE(spinUntil([&] { return store.peek(u"spotify/"_s + kClientId).has_value(); }));
    const auto result = await(auth->signOut());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok());
    EXPECT_EQ(auth->state(), State::SignedOut);
    EXPECT_EQ(signedOut->size(), 1);
    EXPECT_FALSE(store.peek(u"spotify/"_s + kClientId).has_value());
    EXPECT_EQ(await(auth->accessToken())->error().code, ErrorCode::SignInRequired);
}

TEST_F(SpotifyAuthTest, ChangingTheClientIdEndsTheSession)
{
    signInOk();
    auth->setClientId(u"ffffffffffffffffffffffffffffffff"_s);
    EXPECT_EQ(auth->state(), State::SignedOut);
    EXPECT_EQ(signedOut->size(), 1);
    EXPECT_EQ(await(auth->accessToken())->error().code, ErrorCode::SignInRequired);
    // The old ID's stored sign-in is kept.
    EXPECT_TRUE(store.peek(u"spotify/"_s + kClientId).has_value());
    auth->setClientId(QString());
    EXPECT_EQ(auth->state(), State::NotConfigured);
}

TEST_F(SpotifyAuthTest, ASecondSignInCancelsTheFirst)
{
    browserAction = BrowserAction::Nothing;
    const QFuture<Result<void>> first = auth->signIn();
    browserAction = BrowserAction::Approve;
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("A", "R")));
    const QFuture<Result<void>> second = auth->signIn();
    ASSERT_TRUE(spinUntil([&] { return first.isFinished(); }));
    EXPECT_TRUE(first.isCanceled());
    const auto result = await(second);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok());
}

TEST_F(SpotifyAuthTest, CancelSignInStopsListening)
{
    browserAction = BrowserAction::Nothing;
    const QFuture<Result<void>> future = auth->signIn();
    ASSERT_TRUE(spinUntil([&] { return !opened.isEmpty(); }));
    const quint16 port = static_cast<quint16>(
        QUrl(QUrlQuery(opened.first()).queryItemValue(u"redirect_uri"_s, QUrl::FullyDecoded)).port());
    auth->cancelSignIn();
    EXPECT_TRUE(future.isCanceled());
    EXPECT_EQ(auth->state(), State::SignedOut);
    QTcpServer reuse; // the port is free again
    EXPECT_TRUE(reuse.listen(QHostAddress(QHostAddress::LocalHost), port));
}

TEST_F(SpotifyAuthTest, ClientCredentialsForDevelopment)
{
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("CC1", "", 3600, QString())));
    const auto result = await(auth->signInWithClientSecret("SECRET"));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(auth->mode(), SpotifyAuth::Mode::ClientCredentials);
    EXPECT_EQ(auth->state(), State::SignedIn);
    EXPECT_FALSE(auth->hasScopes(SpotifyAuth::playlistScopes()));
    EXPECT_TRUE(auth->hasScopes({}));
    EXPECT_FALSE(auth->signInExpiresAt().has_value());
    const auto request = network.requests().first();
    EXPECT_EQ(formOf(request.body).queryItemValue(u"grant_type"_s), u"client_credentials"_s);
    EXPECT_EQ(request.header("Authorization"),
              QByteArray("Basic " + QByteArray(kClientId.toLatin1() + ":SECRET").toBase64()));
    EXPECT_FALSE(request.body.contains("SECRET"));
    EXPECT_FALSE(store.peek(u"spotify/"_s + kClientId).has_value()); // nothing stored

    advance(3600s); // expired: a new token with the same secret
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("CC2", "", 3600, QString())));
    EXPECT_EQ(await(auth->accessToken())->value(), u"CC2"_s);
    EXPECT_EQ(network.requests().at(1).header("Authorization"), request.header("Authorization"));
    for (const auto &record : pacer.records()) {
        EXPECT_FALSE(record.target.contains(u"SECRET"_s));
    }
}

TEST_F(SpotifyAuthTest, ClientCredentialsWithAWrongSecret)
{
    network.enqueue("POST", kTokenUrl, FakeResponse::json(R"({"error":"invalid_client"})", 400));
    const auto result = await(auth->signInWithClientSecret("WRONG"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotConfigured);
    EXPECT_EQ(auth->state(), State::SignedOut);
    EXPECT_EQ(await(auth->signInWithClientSecret(QByteArray()))->error().code, ErrorCode::NotConfigured);
}

TEST_F(SpotifyAuthTest, StateSignalsFollowTheFlow)
{
    signInOk();
    QList<State> seen;
    for (const auto &arguments : std::as_const(*states)) {
        seen.append(arguments.first().value<State>());
    }
    EXPECT_EQ(seen, (QList<State>{State::Authorizing, State::Exchanging, State::SignedIn}));
}

TEST_F(SpotifyAuthTest, EveryTokenRequestHasAPermitAndAScrubbedRecord)
{
    signInOk();
    EXPECT_EQ(pacer.acquiredCount(), 1);
    ASSERT_EQ(pacer.records().size(), 1);
    EXPECT_FALSE(pacer.records().first().target.contains(u"AUTHCODE"_s));
    EXPECT_TRUE(pacer.records().first().target.contains(u"authorization code"_s));
}
