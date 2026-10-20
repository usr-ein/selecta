// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NetworkService.h"

#include "core/ActivityLog.h"
#include "governor/Governor.h"
#include "governor/ServerGuard.h"
#include "meta/TokenStore.h"

#include <QDateTime>
#include <QHostInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

#include <memory>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcNetwork, "selecta.core.network")

using Event = governor::ReconnectPolicy::Event;
using EndReason = slsk::ServerSession::EndReason;
using State = NetworkStatus::State;

// How a session ended, as the reconnect policy sees it.
std::optional<Event> eventFor(EndReason reason)
{
    switch (reason) {
    case EndReason::Relogged:
        return Event::Relogged;
    case EndReason::LoginRejected:
        return Event::LoginRejected;
    case EndReason::ServerFull:
        return Event::ServerFull;
    case EndReason::LoginTimeout:
        return Event::LoginTimeout;
    case EndReason::InstantEof:
        return Event::InstantEof;
    case EndReason::BanNotice:
        return Event::BanNotice;
    case EndReason::ConnectionLost:
    case EndReason::ConnectFailed: // an ordinary back-off, like a dropped session
        return Event::ConnectionLost;
    case EndReason::ProtocolViolation:
        return Event::ProtocolViolation;
    case EndReason::None:
    case EndReason::LoggedOut:
        break;
    }
    return std::nullopt;
}

QString describe(EndReason reason, const QString &detail)
{
    switch (reason) {
    case EndReason::Relogged:
        return NetworkService::tr("This account logged in somewhere else.");
    case EndReason::LoginRejected:
        return NetworkService::tr("The server refused the login: %1").arg(detail);
    case EndReason::ServerFull:
        return NetworkService::tr("The server is full.");
    case EndReason::LoginTimeout:
        return NetworkService::tr("The server didn't answer the login.");
    case EndReason::InstantEof:
        return NetworkService::tr("The server closed the connection straight away.");
    case EndReason::BanNotice:
        return NetworkService::tr("The server banned this account for a while.");
    case EndReason::ConnectionLost:
        return NetworkService::tr("The connection to the server dropped.");
    case EndReason::ConnectFailed:
        return NetworkService::tr("Couldn't reach the server: %1").arg(detail);
    case EndReason::ProtocolViolation:
        return NetworkService::tr("The server sent something Selecta couldn't read.");
    case EndReason::None:
    case EndReason::LoggedOut:
        break;
    }
    return detail;
}

} // namespace

QString NetworkService::passwordAccount(const QString &username)
{
    return u"soulseek/"_s + username;
}

NetworkService::NetworkService(governor::Governor &governor, ActivityLog &activity, meta::TokenStore &tokens,
                               WallClock now, QObject *parent)
    : QObject(parent)
    , m_governor(governor)
    , m_activity(activity)
    , m_tokens(tokens)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_server(std::make_unique<slsk::ServerSession>())
    , m_retryTimer(new QTimer(this))
{
    qRegisterMetaType<NetworkStatus>();
    m_permits = std::make_unique<GovernorPermits>(m_governor, this);
    m_peers = std::make_unique<slsk::PeerNetwork>(*m_server, *m_permits);
    m_transfers = std::make_unique<slsk::TransferManager>(*m_peers, *m_server);
    m_distributed = std::make_unique<slsk::DistributedNode>(*m_server, *m_peers);
    connect(m_server.get(), &slsk::ServerSession::searchRequestReceived, this,
            [this](const slsk::fromserver::SearchRequest &request) {
                Q_EMIT searchRequestReceived(slsk::IncomingSearch{.username = request.username,
                                                                  .token = request.token,
                                                                  .query = request.query,
                                                                  .route = slsk::IncomingSearch::Route::Server});
            });
    connect(m_distributed.get(), &slsk::DistributedNode::searchRequestReceived, this,
            [this](const slsk::distrib::Search &search) {
                Q_EMIT searchRequestReceived(slsk::IncomingSearch{.username = search.username,
                                                                  .token = search.token,
                                                                  .query = search.query,
                                                                  .route = slsk::IncomingSearch::Route::Distributed});
            });
    connect(m_server.get(), &slsk::ServerSession::privilegedUsersReceived, this,
            &NetworkService::privilegedUsersReceived);
    connect(m_server.get(), &slsk::ServerSession::privateMessageReceived, this,
            [this](const slsk::fromserver::PrivateMessage &message) {
                if (message.username.compare(u"server"_s, Qt::CaseInsensitive) != 0) {
                    Q_EMIT privateMessageReceived(message);
                }
            });
    connect(m_server.get(), &slsk::ServerSession::wishlistIntervalReceived, this, [this](quint32 seconds) {
        // At least two minutes, whatever the server says: a background search is never urgent.
        m_wishlistInterval = std::chrono::seconds(std::max<quint32>(seconds, 120));
        Q_EMIT wishlistIntervalChanged(m_wishlistInterval);
    });
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &NetworkService::attempt);
    connect(m_server.get(), &slsk::ServerSession::loggedIn, this, &NetworkService::loggedIn);
    connect(m_server.get(), &slsk::ServerSession::ended, this, &NetworkService::ended);
    connect(m_server.get(), &slsk::ServerSession::excludedSearchPhrasesReceived, this,
            [this](const QStringList &phrases) { m_governor.setExcludedPhrases(phrases); });
    connect(m_server.get(), &slsk::ServerSession::serverNotice, this, [this](const slsk::ServerNotice &notice) {
        m_activity.record(u"server"_s, QString(), u"notice"_s, notice.text);
    });
    connect(m_peers.get(), &slsk::PeerNetwork::searchResponseReceived, this,
            [this](const QString &username, const slsk::peer::FileSearchResponse &response) {
                if (m_openSearches.contains(response.token)) {
                    Q_EMIT searchResponseReceived(response.token, username, response);
                }
            });
}

bool NetworkService::answerSearch(const slsk::IncomingSearch &search, const QList<slsk::FileEntry> &results,
                                  slsk::Client::UploadAvailability uploads)
{
    if (results.isEmpty() || search.username.isEmpty() || !m_server->isLoggedIn()) {
        return false;
    }
    slsk::peer::FileSearchResponse response;
    response.username = m_server->username();
    response.token = search.token;
    response.results = results;
    response.slotFree = uploads.slotFree;
    response.averageSpeed = uploads.averageSpeed;
    response.queueLength = uploads.queueLength;
    m_peers->sendSearchResponse(search.username, response);
    return true;
}

NetworkService::~NetworkService()
{
    m_retryTimer->stop();
    m_distributed.reset(); // uses the session and the peers
    m_transfers.reset();   // closes open download devices through their storage; emits nothing
    m_peers->closeAll();
    m_server->disconnectFromServer();
    m_peers.reset(); // uses the session and the permits
    m_permits.reset();
    m_server.reset();
}

void NetworkService::log(const QString &outcome, const QString &explanation)
{
    m_activity.record(u"server"_s, m_status.server, outcome, explanation);
}

void NetworkService::setState(State state, const QString &detail)
{
    m_status.state = state;
    m_status.detail = detail;
    m_status.username = m_config.username;
    m_status.server = u"%1:%2"_s.arg(m_config.host).arg(m_config.port);
    if (state != State::Reconnecting) {
        m_status.retryAtMs = 0;
    }
    Q_EMIT statusChanged(m_status);
}

void NetworkService::start(const NetworkConfig &config)
{
    m_config = config;
    m_wanted = config.connectAtLaunch;
    if (m_config.username.isEmpty()) {
        setState(State::NotConfigured, tr("Set your Soulseek account in Settings."));
        return;
    }
    const QPointer<NetworkService> self(this);
    const int generation = ++m_passwordGeneration;
    m_tokens.read(passwordAccount(m_config.username))
        .then(this, [self, generation](const meta::Result<std::optional<QByteArray>> &password) {
            // A password typed (or another account chosen) meanwhile wins over the one read from the store.
            if (!self || generation != self->m_passwordGeneration) {
                return;
            }
            self->m_password = password.ok() && password.value() ? QString::fromUtf8(*password.value()) : QString();
            if (self->m_password.isEmpty()) {
                self->setState(State::NotConfigured, tr("Set your Soulseek password in Settings."));
            } else if (self->m_wanted) {
                self->attempt();
            } else {
                self->setState(State::Offline);
            }
        });
}

void NetworkService::setConfig(const NetworkConfig &config)
{
    if (config == m_config) {
        return;
    }
    const bool reconnect = m_server->state() != slsk::ServerSession::State::Disconnected &&
                           (config.host != m_config.host || config.port != m_config.port ||
                            config.username != m_config.username || config.listenPort != m_config.listenPort);
    const bool userChanged = config.username != m_config.username;
    m_config = config;
    if (config.listenPort != m_status.listenPort && m_status.listening) {
        m_peers->stopListening();
        m_status.listening = false;
    }
    if (userChanged) {
        m_password.clear();
        start(config); // reads the new user's password
        return;
    }
    if (reconnect) {
        m_server->disconnectFromServer();
        attempt();
    }
}

void NetworkService::setPassword(const QString &password)
{
    if (m_config.username.isEmpty()) {
        return;
    }
    m_password = password;
    ++m_passwordGeneration;
    auto future = password.isEmpty() ? m_tokens.remove(passwordAccount(m_config.username))
                                     : m_tokens.write(passwordAccount(m_config.username), password.toUtf8());
    const QPointer<NetworkService> self(this);
    future.then(this, [self](const meta::Result<void> &result) {
        if (self && !result.ok()) {
            qCWarning(lcNetwork).noquote() << "couldn't save the Soulseek password:" << result.error().toString();
        }
    });
    if (password.isEmpty()) {
        disconnectNow();
        setState(State::NotConfigured, tr("Set your Soulseek password in Settings."));
        return;
    }
    connectNow();
}

void NetworkService::connectNow()
{
    m_wanted = true;
    m_governor.serverEvent(Event::UserRequested);
    if (m_server->state() == slsk::ServerSession::State::Disconnected) {
        attempt();
    }
}

void NetworkService::disconnectNow()
{
    m_wanted = false;
    m_retryTimer->stop();
    abortLookup();
    m_peers->closeAll();
    m_server->disconnectFromServer();
    setState(m_config.username.isEmpty() || m_password.isEmpty() ? State::NotConfigured : State::Offline);
}

void NetworkService::attempt()
{
    m_retryTimer->stop();
    if (!m_wanted || m_server->state() != slsk::ServerSession::State::Disconnected) {
        return;
    }
    if (m_config.username.isEmpty() || m_password.isEmpty()) {
        setState(State::NotConfigured, tr("Set your Soulseek account in Settings."));
        return;
    }
    // A public server is refused before any DNS lookup unless this build and the user allow it (SAFE-3).
    if (!governor::serverAllowed(m_config.host, m_config.liveAllowedByUser)) {
        const QString why = governor::liveServerCompiledIn()
                                ? tr("Connecting to the live Soulseek network is turned off in Settings.")
                                : tr("This build connects only to local test servers, not to %1.").arg(m_config.host);
        log(u"live-server-refused"_s, why);
        setState(State::NeedsUser, why);
        return;
    }
    setState(State::Connecting, tr("Connecting to %1…").arg(m_config.host));
    abortLookup();
    QHostAddress literal;
    if (literal.setAddress(m_config.host)) {
        connectTo(literal); // an address needs no lookup
        return;
    }
    // The string-based overload: the functor ones allocate a slot object the static analyser can't follow. The id
    // lets a newer attempt or a disconnect abort a lookup in flight.
    m_lookupId = QHostInfo::lookupHost(m_config.host, this, SLOT(hostLookedUp(QHostInfo)));
}

void NetworkService::abortLookup()
{
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
}

void NetworkService::hostLookedUp(const QHostInfo &info)
{
    if (info.lookupId() != m_lookupId) {
        return; // a newer attempt or a disconnect replaced this one
    }
    m_lookupId = -1;
    const QList<QHostAddress> addresses = info.addresses();
    if (info.error() != QHostInfo::NoError || addresses.isEmpty()) {
        scheduleRetry(30'000, tr("Can't find the server %1: %2").arg(m_config.host, info.errorString()));
        return;
    }
    // Prefer IPv4: Soulseek servers and peers speak it.
    QHostAddress chosen = addresses.constFirst();
    for (const QHostAddress &address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            chosen = address;
            break;
        }
    }
    connectTo(chosen);
}

void NetworkService::connectTo(const QHostAddress &address)
{
    governor::Decision decision = m_governor.request(
        governor::Operation::Login, {.server = address, .liveEnabledByUser = m_config.liveAllowedByUser});
    if (!decision.permit) {
        if (decision.refusal == governor::Refusal::LoginBackoff) {
            scheduleRetry(std::chrono::duration_cast<std::chrono::milliseconds>(decision.retryAfter).count(),
                          decision.explanation);
        } else {
            setState(State::NeedsUser, decision.explanation);
        }
        return;
    }
    ensureListening();
    m_server->setListenPort(m_status.listening ? m_status.listenPort : 0);
    m_server->setSharedCounts(0, 0); // sharing arrives with M7
    if (!m_server->connectToServer(address, m_config.port, std::move(*decision.permit)) ||
        !m_server->login(m_config.username, m_password)) {
        scheduleRetry(30'000, tr("Couldn't start a session with the server."));
    }
}

void NetworkService::ensureListening()
{
    if (m_status.listening && m_status.listenPort == m_config.listenPort) {
        return;
    }
    QString error;
    if (m_config.listenPort != 0 && m_peers->listen(m_config.listenPort, &error)) {
        m_status.listening = true;
        m_status.listenPort = m_config.listenPort;
    } else {
        // Without a listening port, peers reach us only through the server (indirect connections): slower, but it
        // works.
        m_status.listening = false;
        m_status.listenPort = 0;
        qCWarning(lcNetwork).noquote() << "not listening for peers:" << error;
    }
}

void NetworkService::loggedIn(const slsk::LoginInfo &info)
{
    m_governor.serverEvent(Event::LoginSucceeded);
    log(u"connected"_s, tr("Logged in as %1.").arg(info.username));
    setState(State::Online, info.greeting);
}

void NetworkService::ended(EndReason reason, const QString &detail)
{
    m_openSearches.clear();
    const QString why = describe(reason, detail);
    if (reason == EndReason::LoggedOut) {
        log(u"disconnected"_s, tr("Disconnected."));
        return;
    }
    if (const auto event = eventFor(reason)) {
        m_governor.serverEvent(*event);
    }
    log(u"ended"_s, why);
    if (!m_wanted) {
        setState(State::Offline, why);
        return;
    }
    if (m_governor.loginNeedsUser()) {
        setState(State::NeedsUser, why);
        return;
    }
    // On the next turn, once the session has finished ending; the governor answers with the back-off, which
    // schedules the retry.
    QMetaObject::invokeMethod(this, &NetworkService::attempt, Qt::QueuedConnection);
}

void NetworkService::scheduleRetry(qint64 delayMs, const QString &detail)
{
    const qint64 delay = std::max<qint64>(1000, delayMs);
    m_status.retryAtMs = m_now() + delay; // kept by setState for this state
    setState(State::Reconnecting, detail);
    m_retryTimer->start(std::chrono::milliseconds(delay));
}

quint32 NetworkService::search(const QString &query, governor::Permit permit)
{
    if (!m_server->isLoggedIn()) {
        return 0;
    }
    const quint32 token = m_searchTokens.next([this](quint32 candidate) { return m_openSearches.contains(candidate); });
    if (token == 0 || !m_server->fileSearch(token, query, std::move(permit))) {
        return 0;
    }
    m_openSearches.insert(token);
    m_peers->openSearch(token);
    return token;
}

bool NetworkService::sendPrivateMessage(const QString &username, const QString &text, governor::Permit permit)
{
    return m_server->isLoggedIn() && m_server->sendPrivateMessage(username, text, std::move(permit));
}

quint32 NetworkService::searchWishlist(const QString &query, governor::Permit permit)
{
    if (!m_server->isLoggedIn()) {
        return 0;
    }
    const quint32 token = m_searchTokens.next([this](quint32 candidate) { return m_openSearches.contains(candidate); });
    if (token == 0 || !m_server->wishlistSearch(token, query, std::move(permit))) {
        return 0;
    }
    m_openSearches.insert(token);
    m_peers->openSearch(token);
    return token;
}

void NetworkService::stopSearch(quint32 token)
{
    if (m_openSearches.remove(token)) {
        m_peers->closeSearch(token);
    }
}

GovernorPermits::GovernorPermits(governor::Governor &governor, QObject *context)
    : m_governor(governor)
    , m_context(context)
{}

void GovernorPermits::requestPeerLookup(const QString &username, slsk::ConnectPurpose purpose, Callback deliver)
{
    Q_UNUSED(purpose) // one budget for every purpose for now (governor Limits: peer lookups)
    governor::Decision decision = m_governor.request(governor::Operation::PeerLookup, {.peer = username});
    if (!m_context) {
        return;
    }
    // A Permit is move-only; the shared holder keeps the queued call copyable. Delivered on a later turn, as slsk asks.
    const auto permit = std::make_shared<std::optional<governor::Permit>>(std::move(decision.permit));
    QMetaObject::invokeMethod(
        m_context.data(), [deliver = std::move(deliver), permit] { deliver(std::move(*permit)); },
        Qt::QueuedConnection);
}

} // namespace selecta::core
