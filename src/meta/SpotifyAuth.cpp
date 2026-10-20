// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/SpotifyAuth.h"

#include "meta/Pkce.h"
#include "meta/RequestPacer.h"
#include "meta/TokenStore.h"
#include "meta/detail/Async.h"
#include "meta/detail/HttpChannel.h"
#include "meta/detail/Json.h"
#include "meta/detail/LoopbackReceiver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>
#include <QUrlQuery>

#include <vector>

namespace {
Q_LOGGING_CATEGORY(lcMetaSpotifyAuth, "selecta.meta.spotify.auth")
} // namespace

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::meta {

namespace {

constexpr int kStoredRecordVersion = 1;
constexpr qsizetype kMaxTokenLength = 4096;

// application/x-www-form-urlencoded, every value percent-encoded (so '+' can't turn into a space).
QByteArray formBody(const QList<std::pair<QByteArray, QByteArray>> &fields)
{
    QByteArray body;
    for (const auto &[key, value] : fields) {
        if (!body.isEmpty()) {
            body += '&';
        }
        body += key + '=' + QUrl::toPercentEncoding(QString::fromUtf8(value));
    }
    return body;
}

struct TokenAnswer
{
    QByteArray accessToken;
    std::chrono::seconds expiresIn{3600};
    QByteArray refreshToken; // may be absent on refresh: keep the old one then
    std::optional<QStringList> scopes;
};

enum class TokenGrant : quint8 {
    AuthorizationCode,
    Refresh,
    ClientCredentials,
};

MetaError authError(ErrorCode code, QString message, int status = 0)
{
    return makeError(Service::SpotifyAccounts, code, std::move(message), status);
}

// Reads a token endpoint answer (RFC 6749 §5.1 / §5.2).
Result<TokenAnswer> readTokenAnswer(const Result<detail::HttpResponse> &result, TokenGrant grant)
{
    if (!result.ok()) {
        return result.error();
    }
    const detail::HttpResponse &response = result.value();
    if (response.status == 400 || response.status == 401) {
        const Result<QJsonObject> json = detail::json::parseObject(response.body, Service::SpotifyAccounts);
        const QString code = json.ok() ? detail::json::text(json.value(), "error"_L1, 64) : QString();
        if (code == u"invalid_client"_s) {
            return authError(ErrorCode::NotConfigured,
                             grant == TokenGrant::ClientCredentials
                                 ? u"Spotify rejected the Client ID or the client secret."_s
                                 : u"Spotify doesn't accept this Client ID. Check it in Settings → Integrations."_s,
                             response.status);
        }
        if (code == u"invalid_grant"_s) {
            if (grant == TokenGrant::Refresh) {
                return authError(ErrorCode::SignInRequired,
                                 u"Your Spotify sign-in has expired or was revoked. Sign in again."_s, response.status);
            }
            return authError(ErrorCode::BadResponse, u"Spotify didn't accept the sign-in code. Try signing in again."_s,
                             response.status);
        }
        return authError(ErrorCode::BadResponse,
                         u"Spotify refused the token request (%1)."_s.arg(code.isEmpty() ? u"no reason"_s : code),
                         response.status);
    }
    if (response.status != 200) {
        return detail::statusError(Service::SpotifyAccounts, response);
    }
    const Result<QJsonObject> json = detail::json::parseObject(response.body, Service::SpotifyAccounts);
    if (!json.ok()) {
        return json.error();
    }
    const QJsonObject &object = json.value();
    TokenAnswer answer;
    const QString accessToken = object.value("access_token"_L1).toString();
    if (accessToken.isEmpty() || accessToken.size() > kMaxTokenLength) {
        return authError(ErrorCode::BadResponse, u"Spotify's token answer has no usable access token."_s, 200);
    }
    if (object.value("token_type"_L1).toString().compare(u"Bearer"_s, Qt::CaseInsensitive) != 0) {
        return authError(ErrorCode::BadResponse, u"Spotify's token answer isn't a bearer token."_s, 200);
    }
    answer.accessToken = accessToken.toUtf8();
    if (const std::optional<int> expires = detail::json::positiveInt(object, "expires_in"_L1)) {
        answer.expiresIn = std::chrono::seconds(*expires);
    }
    const QString refreshToken = object.value("refresh_token"_L1).toString();
    if (refreshToken.size() > kMaxTokenLength) {
        return authError(ErrorCode::BadResponse, u"Spotify's refresh token is implausibly long."_s, 200);
    }
    answer.refreshToken = refreshToken.toUtf8();
    if (grant == TokenGrant::AuthorizationCode && answer.refreshToken.isEmpty()) {
        return authError(ErrorCode::BadResponse, u"Spotify's token answer has no refresh token."_s, 200);
    }
    if (object.contains("scope"_L1)) {
        answer.scopes = detail::json::text(object, "scope"_L1).split(u' ', Qt::SkipEmptyParts);
    }
    return answer;
}

} // namespace

struct SpotifyAuth::Private
{
    // A sign-in in progress. A flow can end from inside a signal of its own receiver or timer, so those two are
    // deleted later, never under their own feet; they are children of the SpotifyAuth meanwhile.
    struct Flow
    {
        Flow() = default;
        Flow(const Flow &) = delete;
        Flow &operator=(const Flow &) = delete;
        Flow(Flow &&) = delete;
        Flow &operator=(Flow &&) = delete;
        ~Flow()
        {
            if (receiver != nullptr) {
                receiver->disconnect();
                receiver->close();
                receiver->deleteLater();
            }
            if (timeout != nullptr) {
                timeout->disconnect();
                timeout->stop();
                timeout->deleteLater();
            }
        }

        quint64 id = 0;
        detail::SharedPromise<Result<void>> promise;
        detail::LoopbackReceiver *receiver = nullptr;
        QTimer *timeout = nullptr;
        QByteArray verifier;
        QUrl redirectUri;
        bool exchanging = false;
        detail::HttpChannel::RequestId request = 0;
    };

    // Grouped by size to avoid padding.
    SpotifyAuth *q;
    TokenStore &tokens;
    const base::Clock &clock;
    std::unique_ptr<detail::HttpChannel> channel;
    std::unique_ptr<Flow> flow; // a sign-in in progress
    quint64 nextFlowId = 1;
    quint64 generation = 0; // bumps whenever the session changes; late answers for an older one are dropped
    detail::HttpChannel::RequestId refreshRequest = 0;
    base::Clock::TimePoint accessExpires;
    QDateTime authorisedAt;
    // The session's secrets: never logged.
    QByteArray refreshToken;
    QByteArray clientSecret;
    QByteArray accessToken;
    QStringList scopes;
    std::vector<detail::SharedPromise<Result<void>>> restoreWaiters;
    std::vector<detail::SharedPromise<Result<QString>>> tokenWaiters;
    Config config;
    Mode mode = Mode::None;
    State lastState = State::NotConfigured;
    bool expired = false;
    bool restoring = false;
    bool refreshing = false;

    Private(SpotifyAuth *owner, Config cfg, QNetworkAccessManager *network, RequestPacer &pacer, TokenStore &store,
            const base::Clock &c)
        : q(owner)
        , tokens(store)
        , clock(c)
        , channel(std::make_unique<detail::HttpChannel>(Service::SpotifyAccounts, network, pacer, nullptr, c))
        , config(std::move(cfg))
    {}

    QString account() const { return u"spotify/"_s + config.clientId; }

    State computeState() const
    {
        if (config.clientId.isEmpty()) {
            return State::NotConfigured;
        }
        if (flow) {
            return flow->exchanging ? State::Exchanging : State::Authorizing;
        }
        if (restoring) {
            return State::Restoring;
        }
        if (mode != Mode::None) {
            return State::SignedIn;
        }
        return expired ? State::Expired : State::SignedOut;
    }

    void updateState()
    {
        const State state = computeState();
        if (state != lastState) {
            lastState = state;
            Q_EMIT q->stateChanged(state);
        }
    }

    MetaError signInRequired() const
    {
        return authError(ErrorCode::SignInRequired, expired ? u"Your Spotify sign-in has expired. Sign in again."_s
                                                            : u"Not signed in to Spotify."_s);
    }

    void failTokenWaiters(const MetaError &error)
    {
        const std::vector<detail::SharedPromise<Result<QString>>> waiters = std::exchange(tokenWaiters, {});
        for (const auto &waiter : waiters) {
            waiter.finish(error);
        }
    }

    void finishRestoreWaiters(const Result<void> &result)
    {
        const std::vector<detail::SharedPromise<Result<void>>> waiters = std::exchange(restoreWaiters, {});
        for (const auto &waiter : waiters) {
            waiter.finish(result);
        }
    }

    // Ends the session in memory. `stored` also deletes the stored refresh token.
    void endSession(bool stored, bool nowExpired, const MetaError &reason)
    {
        const bool hadSession = mode != Mode::None;
        ++generation;
        if (refreshRequest != 0) {
            channel->cancel(std::exchange(refreshRequest, 0));
        }
        refreshing = false;
        failTokenWaiters(reason);
        mode = Mode::None;
        refreshToken.clear();
        clientSecret.clear();
        accessToken.clear();
        authorisedAt = {};
        scopes.clear();
        expired = nowExpired;
        if (stored && !config.clientId.isEmpty()) {
            removeStored();
        }
        if (hadSession || nowExpired) {
            Q_EMIT q->signedOut();
        }
        updateState();
    }

    void removeStored()
    {
        detail::whenReady(tokens.remove(account()), q, [](const Result<void> &result) {
            if (!result.ok()) {
                qCWarning(lcMetaSpotifyAuth).noquote()
                    << "couldn't delete the stored Spotify sign-in:" << result.error().toString();
            }
        });
    }

    void cancelFlow()
    {
        if (!flow) {
            return;
        }
        const std::unique_ptr<Flow> old = std::move(flow);
        if (old->request != 0) {
            channel->cancel(old->request);
        }
        old->promise.cancel();
        updateState();
    }

    void finishFlow(const Result<void> &result)
    {
        if (!flow) {
            return;
        }
        const std::unique_ptr<Flow> done = std::move(flow);
        updateState();
        done->promise.finish(result);
    }

    static QString grantName(TokenGrant grant)
    {
        switch (grant) {
        case TokenGrant::AuthorizationCode:
            return u"authorization code"_s;
        case TokenGrant::Refresh:
            return u"refresh"_s;
        case TokenGrant::ClientCredentials:
            return u"client credentials"_s;
        }
        return {};
    }

    // POSTs to the token endpoint. `secret` (client credentials only) goes into the Basic Authorization header.
    detail::HttpChannel::RequestId requestToken(const QList<std::pair<QByteArray, QByteArray>> &form, TokenGrant grant,
                                                std::function<void(Result<TokenAnswer>)> done,
                                                const QByteArray &secret = {})
    {
        detail::HttpRequest request;
        request.method = QByteArrayLiteral("POST");
        request.url = config.accountsUrl;
        request.url.setPath(request.url.path() + u"/api/token"_s);
        request.headers.append(
            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/x-www-form-urlencoded")});
        request.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")});
        if (grant == TokenGrant::ClientCredentials) {
            request.headers.append(
                {QByteArrayLiteral("Authorization"),
                 QByteArrayLiteral("Basic ") + QByteArray(config.clientId.toUtf8() + ':' + secret).toBase64()});
        }
        request.body = formBody(form);
        request.logTarget = u"%1/api/token (%2)"_s.arg(config.accountsUrl.host(), grantName(grant));
        const auto callback = std::make_shared<std::function<void(Result<TokenAnswer>)>>(std::move(done));
        return channel->send(std::move(request), [grant, callback](const Result<detail::HttpResponse> &result) {
            (*callback)(readTokenAnswer(result, grant));
        });
    }

    void persist()
    {
        if (mode != Mode::User || config.clientId.isEmpty()) {
            return;
        }
        QJsonObject record;
        record.insert("v"_L1, kStoredRecordVersion);
        record.insert("client_id"_L1, config.clientId);
        record.insert("refresh_token"_L1, QString::fromUtf8(refreshToken));
        record.insert("authorised_at"_L1, authorisedAt.toString(Qt::ISODate));
        record.insert("scopes"_L1, QJsonArray::fromStringList(scopes));
        const QByteArray secret = QJsonDocument(record).toJson(QJsonDocument::Compact);
        detail::whenReady(tokens.write(account(), secret), q, [this](const Result<void> &result) {
            if (!result.ok()) {
                qCWarning(lcMetaSpotifyAuth).noquote()
                    << "couldn't save the Spotify sign-in:" << result.error().toString();
                Q_EMIT q->persistFailed(result.error());
            }
        });
    }

    void refresh()
    {
        if (refreshing || mode == Mode::None) {
            return;
        }
        refreshing = true;
        const quint64 g = generation;
        auto onAnswer = [this, g](Result<TokenAnswer> answer) {
            if (g != generation) {
                return; // the session changed meanwhile; its waiters were already answered
            }
            refreshing = false;
            refreshRequest = 0;
            if (!answer.ok()) {
                const MetaError &error = answer.error();
                if (error.code == ErrorCode::SignInRequired) {
                    endSession(true, true, error);
                } else if (error.code == ErrorCode::NotConfigured && mode == Mode::ClientCredentials) {
                    endSession(false, false, error);
                } else {
                    failTokenWaiters(error); // transient: the sign-in stays, the next call retries
                }
                return;
            }
            TokenAnswer &value = answer.value();
            accessToken = value.accessToken;
            accessExpires = clock.now() + std::chrono::duration_cast<base::Clock::Duration>(value.expiresIn);
            if (value.scopes.has_value() && mode == Mode::User) {
                scopes = *value.scopes;
            }
            if (mode == Mode::User && !value.refreshToken.isEmpty() && value.refreshToken != refreshToken) {
                refreshToken = value.refreshToken; // rotated; otherwise keep the old one (Spotify may not rotate)
                persist();
            }
            const QString token = QString::fromUtf8(accessToken);
            const std::vector<detail::SharedPromise<Result<QString>>> waiters = std::exchange(tokenWaiters, {});
            for (const auto &waiter : waiters) {
                waiter.finish(token);
            }
        };
        if (mode == Mode::ClientCredentials) {
            refreshRequest = requestToken({{QByteArrayLiteral("grant_type"), QByteArrayLiteral("client_credentials")}},
                                          TokenGrant::ClientCredentials, std::move(onAnswer), clientSecret);
            return;
        }
        refreshRequest = requestToken({{QByteArrayLiteral("grant_type"), QByteArrayLiteral("refresh_token")},
                                       {QByteArrayLiteral("refresh_token"), refreshToken},
                                       {QByteArrayLiteral("client_id"), config.clientId.toUtf8()}},
                                      TokenGrant::Refresh, std::move(onAnswer));
    }

    void onCode(quint64 flowId, const QByteArray &code)
    {
        if (!flow || flow->id != flowId) {
            return;
        }
        flow->exchanging = true;
        if (flow->timeout != nullptr) {
            flow->timeout->stop();
        }
        updateState();
        flow->request =
            requestToken({{QByteArrayLiteral("grant_type"), QByteArrayLiteral("authorization_code")},
                          {QByteArrayLiteral("code"), code},
                          {QByteArrayLiteral("redirect_uri"), flow->redirectUri.toString(QUrl::FullyEncoded).toUtf8()},
                          {QByteArrayLiteral("client_id"), config.clientId.toUtf8()},
                          {QByteArrayLiteral("code_verifier"), flow->verifier}},
                         TokenGrant::AuthorizationCode, [this, flowId](Result<TokenAnswer> answer) {
                             if (!flow || flow->id != flowId) {
                                 return;
                             }
                             flow->request = 0;
                             if (!answer.ok()) {
                                 finishFlow(answer.error());
                                 return;
                             }
                             startSession(Mode::User, answer.value(), {});
                             persist();
                             finishFlow({});
                         });
    }

    void startSession(Mode newMode, const TokenAnswer &answer, const QByteArray &secret)
    {
        ++generation;
        if (refreshRequest != 0) {
            channel->cancel(std::exchange(refreshRequest, 0));
        }
        refreshing = false;
        mode = newMode;
        expired = false;
        accessToken = answer.accessToken;
        accessExpires = clock.now() + std::chrono::duration_cast<base::Clock::Duration>(answer.expiresIn);
        refreshToken = answer.refreshToken;
        clientSecret = secret;
        scopes = newMode == Mode::User ? answer.scopes.value_or(QStringList{}) : QStringList{};
        authorisedAt = newMode == Mode::User ? QDateTime::currentDateTimeUtc() : QDateTime();
        // Callers waiting for a token from the previous session get the new one.
        const QString token = QString::fromUtf8(accessToken);
        const std::vector<detail::SharedPromise<Result<QString>>> waiters = std::exchange(tokenWaiters, {});
        for (const auto &waiter : waiters) {
            waiter.finish(token);
        }
        updateState();
    }
};

QStringList SpotifyAuth::playlistScopes()
{
    return {u"playlist-read-private"_s, u"playlist-read-collaborative"_s};
}

QStringList SpotifyAuth::libraryScopes()
{
    return {u"user-library-read"_s};
}

SpotifyAuth::SpotifyAuth(Config config, QNetworkAccessManager *network, RequestPacer &pacer, TokenStore &tokens,
                         const base::Clock &clock, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, std::move(config), network, pacer, tokens, clock))
{
    d->config.clientId = d->config.clientId.trimmed();
    d->lastState = d->computeState();
}

SpotifyAuth::~SpotifyAuth()
{
    d->flow.reset(); // stops listening; its future ends canceled
}

SpotifyAuth::State SpotifyAuth::state() const
{
    return d->computeState();
}

SpotifyAuth::Mode SpotifyAuth::mode() const
{
    return d->mode;
}

QString SpotifyAuth::clientId() const
{
    return d->config.clientId;
}

QStringList SpotifyAuth::grantedScopes() const
{
    return d->scopes;
}

bool SpotifyAuth::hasScopes(const QStringList &scopes) const
{
    if (d->mode != Mode::User) {
        return scopes.isEmpty() && d->mode != Mode::None;
    }
    return std::ranges::all_of(scopes, [this](const QString &scope) { return d->scopes.contains(scope); });
}

std::optional<QDateTime> SpotifyAuth::signInExpiresAt() const
{
    if (d->mode != Mode::User || !d->authorisedAt.isValid()) {
        return std::nullopt;
    }
    return d->authorisedAt.addMonths(kSignInLifetimeMonths);
}

void SpotifyAuth::setClientId(const QString &clientId)
{
    const QString trimmed = clientId.trimmed();
    if (trimmed == d->config.clientId) {
        return;
    }
    d->cancelFlow();
    if (d->restoring) {
        d->restoring = false;
        d->finishRestoreWaiters(authError(ErrorCode::NotConfigured, u"The Spotify Client ID changed."_s));
    }
    d->endSession(false, false, authError(ErrorCode::SignInRequired, u"The Spotify Client ID changed."_s));
    d->config.clientId = trimmed;
    d->updateState();
}

QFuture<Result<void>> SpotifyAuth::restore()
{
    if (d->config.clientId.isEmpty()) {
        return detail::deferred<Result<void>>(this, authError(ErrorCode::NotConfigured, u"No Spotify Client ID."_s));
    }
    if (d->mode != Mode::None) {
        return detail::deferred<Result<void>>(this, Result<void>());
    }
    const detail::SharedPromise<Result<void>> promise;
    d->restoreWaiters.push_back(promise);
    if (d->restoring) {
        return promise.future();
    }
    d->restoring = true;
    d->updateState();
    const quint64 g = d->generation;
    const QString clientId = d->config.clientId;
    detail::whenReady(d->tokens.read(d->account()), this,
                      [this, g, clientId](const Result<std::optional<QByteArray>> &result) {
                          if (!d->restoring || g != d->generation || clientId != d->config.clientId) {
                              return;
                          }
                          d->restoring = false;
                          if (!result.ok()) {
                              d->updateState();
                              d->finishRestoreWaiters(result.error());
                              return;
                          }
                          if (!result.value().has_value()) {
                              d->updateState();
                              d->finishRestoreWaiters({});
                              return;
                          }
                          const QJsonDocument document = QJsonDocument::fromJson(*result.value());
                          const QJsonObject record = document.object();
                          const QByteArray refreshToken = record.value("refresh_token"_L1).toString().toUtf8();
                          const QDateTime authorisedAt =
                              QDateTime::fromString(record.value("authorised_at"_L1).toString(), Qt::ISODate);
                          if (record.value("v"_L1).toInt() != kStoredRecordVersion || refreshToken.isEmpty() ||
                              refreshToken.size() > kMaxTokenLength || !authorisedAt.isValid() ||
                              record.value("client_id"_L1).toString() != clientId) {
                              qCWarning(lcMetaSpotifyAuth) << "the stored Spotify sign-in is unreadable; deleting it";
                              d->removeStored();
                              d->updateState();
                              d->finishRestoreWaiters({});
                              return;
                          }
                          if (authorisedAt.addMonths(kSignInLifetimeMonths) <= QDateTime::currentDateTimeUtc()) {
                              d->removeStored();
                              d->expired = true;
                              Q_EMIT signedOut();
                              d->updateState();
                              d->finishRestoreWaiters({});
                              return;
                          }
                          ++d->generation;
                          d->mode = Mode::User;
                          d->expired = false;
                          d->refreshToken = refreshToken;
                          d->authorisedAt = authorisedAt.toUTC();
                          d->scopes.clear();
                          const QJsonArray scopes = record.value("scopes"_L1).toArray();
                          for (const auto &scope : scopes) {
                              if (scope.isString() && d->scopes.size() < detail::json::kMaxListItems) {
                                  d->scopes.append(scope.toString());
                              }
                          }
                          d->accessToken.clear();
                          d->updateState();
                          d->finishRestoreWaiters({});
                      });
    return promise.future();
}

QFuture<Result<void>> SpotifyAuth::signIn(const QStringList &scopes)
{
    if (d->config.clientId.isEmpty()) {
        return detail::deferred<Result<void>>(
            this, authError(ErrorCode::NotConfigured, u"Enter your Spotify Client ID in Settings → Integrations."_s));
    }
    if (!d->config.openUrl) {
        return detail::deferred<Result<void>>(
            this, authError(ErrorCode::NotConfigured, u"Selecta can't open a browser for the sign-in."_s));
    }
    d->cancelFlow();

    auto flow = std::make_unique<Private::Flow>();
    flow->id = d->nextFlowId++;
    flow->verifier = pkce::makeVerifier();
    const QByteArray state = pkce::makeState();
    flow->receiver = new detail::LoopbackReceiver(state, this);
    flow->timeout = new QTimer(this);
    if (const Result<void> listening = flow->receiver->listen(d->config.callbackPort); !listening.ok()) {
        return detail::deferred<Result<void>>(this, listening.error());
    }
    flow->redirectUri = flow->receiver->redirectUri();

    QStringList wanted = d->mode == Mode::User ? d->scopes : QStringList{};
    for (const QString &scope : scopes) {
        if (!wanted.contains(scope)) {
            wanted.append(scope);
        }
    }
    QUrl url = d->config.accountsUrl;
    url.setPath(url.path() + u"/authorize"_s);
    QUrlQuery query;
    query.addQueryItem(u"client_id"_s, d->config.clientId);
    query.addQueryItem(u"response_type"_s, u"code"_s);
    query.addQueryItem(u"redirect_uri"_s, flow->redirectUri.toString(QUrl::FullyEncoded));
    query.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
    query.addQueryItem(u"code_challenge"_s, QString::fromLatin1(pkce::challengeS256(flow->verifier)));
    query.addQueryItem(u"state"_s, QString::fromLatin1(state));
    if (!wanted.isEmpty()) {
        query.addQueryItem(u"scope"_s, wanted.join(u' '));
    }
    url.setQuery(query.query(QUrl::FullyEncoded), QUrl::StrictMode);

    const quint64 flowId = flow->id;
    connect(flow->receiver, &detail::LoopbackReceiver::codeReceived, this,
            [this, flowId](const QByteArray &code) { d->onCode(flowId, code); });
    connect(flow->receiver, &detail::LoopbackReceiver::failed, this, [this, flowId](const MetaError &error) {
        if (d->flow && d->flow->id == flowId) {
            d->finishFlow(error);
        }
    });
    flow->timeout->setSingleShot(true);
    connect(flow->timeout, &QTimer::timeout, this, [this, flowId] {
        if (d->flow && d->flow->id == flowId && !d->flow->exchanging) {
            d->finishFlow(
                authError(ErrorCode::Timeout, u"The Spotify sign-in wasn't completed in the browser in time."_s));
        }
    });
    flow->timeout->start(d->config.callbackTimeout);
    const QFuture<Result<void>> future = flow->promise.future();
    d->flow = std::move(flow);
    d->updateState();

    if (!d->config.openUrl(url)) {
        const detail::SharedPromise<Result<void>> promise = d->flow->promise;
        d->flow.reset();
        d->updateState();
        promise.finishLater(this, authError(ErrorCode::NotConfigured, u"Selecta couldn't open your browser."_s));
    }
    return future;
}

void SpotifyAuth::cancelSignIn()
{
    d->cancelFlow();
}

QFuture<Result<void>> SpotifyAuth::signInWithClientSecret(const QByteArray &clientSecret)
{
    if (d->config.clientId.isEmpty() || clientSecret.isEmpty()) {
        return detail::deferred<Result<void>>(
            this, authError(ErrorCode::NotConfigured, u"The client-credentials flow needs a Client ID and secret."_s));
    }
    d->cancelFlow();
    auto flow = std::make_unique<Private::Flow>();
    flow->id = d->nextFlowId++;
    flow->exchanging = true;
    const quint64 flowId = flow->id;
    const QFuture<Result<void>> future = flow->promise.future();
    d->flow = std::move(flow);
    d->updateState();
    d->flow->request = d->requestToken(
        {{QByteArrayLiteral("grant_type"), QByteArrayLiteral("client_credentials")}}, TokenGrant::ClientCredentials,
        [this, flowId, clientSecret](Result<TokenAnswer> answer) {
            if (!d->flow || d->flow->id != flowId) {
                return;
            }
            d->flow->request = 0;
            if (!answer.ok()) {
                d->finishFlow(answer.error());
                return;
            }
            d->startSession(Mode::ClientCredentials, answer.value(), clientSecret);
            d->finishFlow({});
        },
        clientSecret);
    return future;
}

QFuture<Result<void>> SpotifyAuth::signOut()
{
    d->cancelFlow();
    if (d->config.clientId.isEmpty()) {
        d->endSession(false, false, d->signInRequired());
        return detail::deferred<Result<void>>(this, Result<void>());
    }
    d->endSession(false, false, authError(ErrorCode::SignInRequired, u"Signed out of Spotify."_s));
    const detail::SharedPromise<Result<void>> promise;
    detail::whenReady(d->tokens.remove(d->account()), this,
                      [promise](const Result<void> &result) { promise.finish(result); });
    return promise.future();
}

QFuture<Result<QString>> SpotifyAuth::accessToken()
{
    if (d->mode == Mode::None) {
        return detail::deferred<Result<QString>>(this, d->signInRequired());
    }
    if (!d->accessToken.isEmpty() && d->clock.now() + kRefreshMargin < d->accessExpires) {
        return detail::deferred<Result<QString>>(this, QString::fromUtf8(d->accessToken));
    }
    const detail::SharedPromise<Result<QString>> waiter;
    d->tokenWaiters.push_back(waiter);
    d->refresh();
    return waiter.future();
}

void SpotifyAuth::reportUnauthorized(const QString &token)
{
    if (!d->accessToken.isEmpty() && pkce::constantTimeEquals(token.toUtf8(), d->accessToken)) {
        d->accessToken.clear();
    }
}

} // namespace selecta::meta
