// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QFuture>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace selecta::meta {

class RequestPacer;
class TokenStore;

// Spotify sign-in without a secret (META-1, D21): Authorization Code with PKCE (RFC 7636, S256) and the user's own
// Client ID. The browser answers on a one-shot loopback listener, http://127.0.0.1:<port>/callback, where <port> is
// random unless the user registered a fixed one. Access tokens (1 h) stay in memory and are refreshed 5 minutes
// before they expire; the refresh token goes to the TokenStore (the Keychain in the app). Spotify ends a sign-in
// 6 months after consent (refreshing doesn't extend it); the next refresh then answers `invalid_grant` and the state
// becomes Expired until the user signs in again.
//
// For development only, signInWithClientSecret() uses the client-credentials flow (catalogue lookups, no user data)
// with a secret the caller holds; meta never stores it.
//
// Threading: lives on the thread that owns the meta objects and must only be used there. Nothing blocks: the
// Keychain, the browser and Spotify are all waited for asynchronously. Futures finish on this object's thread.
//
// Ownership: the network manager, pacer and token store must outlive this object; they are not owned.
//
// Failure modes: every operation reports through its future (see each function); the state moves to SignedOut or
// Expired when the sign-in is gone, and stays SignedIn when a refresh fails for a transient reason (network, 5xx,
// rate limit), so the next call retries.
class SpotifyAuth : public QObject
{
    Q_OBJECT

public:
    enum class State : quint8 {
        NotConfigured, // no Client ID
        SignedOut,     // a Client ID, no sign-in (or not restored yet)
        Restoring,     // reading the stored sign-in
        Authorizing,   // waiting for the user in the browser
        Exchanging,    // trading the browser's code for tokens
        SignedIn,      // a usable sign-in
        Expired,       // the sign-in expired or was revoked: sign in again
    };
    Q_ENUM(State)

    enum class Mode : quint8 {
        None,
        User,              // PKCE: user data and the catalogue
        ClientCredentials, // development: catalogue only
    };

    struct Config
    {
        QString clientId;
        quint16 callbackPort = 0;                 // 0: a random free port (register http://127.0.0.1/callback)
        std::chrono::seconds callbackTimeout{300}; // how long the browser step may take
        // Opens the authorisation page in the user's browser (the app passes QDesktopServices::openUrl, so meta
        // needs no Qt Gui). Returns false when it couldn't. The URL carries the Client ID: don't log it.
        std::function<bool(const QUrl &)> openUrl;
        QUrl accountsUrl{QStringLiteral("https://accounts.spotify.com")}; // tests point it at a fake
    };

    // The scopes each feature needs (asked for only when the feature is first used).
    static QStringList playlistScopes();  // playlist-read-private, playlist-read-collaborative
    static QStringList libraryScopes();   // user-library-read (Liked Songs)

    // Access tokens are refreshed when they expire within this margin.
    static constexpr std::chrono::minutes kRefreshMargin{5};
    // Spotify ends a PKCE sign-in this long after consent.
    static constexpr int kSignInLifetimeMonths = 6;

    SpotifyAuth(Config config, QNetworkAccessManager *network, RequestPacer &pacer, TokenStore &tokens,
                const base::Clock &clock, QObject *parent = nullptr);
    ~SpotifyAuth() override;
    SpotifyAuth(const SpotifyAuth &) = delete;
    SpotifyAuth &operator=(const SpotifyAuth &) = delete;
    SpotifyAuth(SpotifyAuth &&) = delete;
    SpotifyAuth &operator=(SpotifyAuth &&) = delete;

    State state() const;
    Mode mode() const;
    QString clientId() const;
    QStringList grantedScopes() const;
    bool hasScopes(const QStringList &scopes) const;
    // When Spotify will end the current user sign-in (consent + 6 months); empty when there is none.
    std::optional<QDateTime> signInExpiresAt() const;

    // Changes the Client ID. A different ID ends the session in memory (stored sign-ins of other IDs are kept) and
    // cancels a sign-in in progress; call restore() afterwards to load the new ID's stored sign-in.
    void setClientId(const QString &clientId);

    // Loads the stored sign-in for the current Client ID. Finishes with success when one was found (SignedIn), or
    // when there was none (SignedOut). Fails with NotConfigured (no Client ID) or the store's error. A stored sign-in
    // older than 6 months is deleted and the state becomes Expired (success, nothing to restore).
    QFuture<Result<void>> restore();

    // Signs in through the browser, asking for `scopes` plus those already granted. Finishes when the tokens are in
    // hand. Fails with NotConfigured (no Client ID or no openUrl), PortUnavailable, AccessDenied (declined in the
    // browser), StateMismatch, Timeout (no answer within callbackTimeout), or the token endpoint's error. Starting a
    // new sign-in cancels one in progress (its future ends canceled). A current sign-in stays usable meanwhile.
    QFuture<Result<void>> signIn(const QStringList &scopes = {});
    void cancelSignIn();

    // Development only: client-credentials flow with `clientSecret` (held in memory for the session, never stored).
    // Catalogue lookups only; playlist imports fail with MissingScope.
    QFuture<Result<void>> signInWithClientSecret(const QByteArray &clientSecret);

    // Forgets the sign-in: tokens in memory and in the store. Emits signedOut(). Fails only when the store can't
    // delete the refresh token (the sign-in is gone from memory anyway).
    QFuture<Result<void>> signOut();

    // A valid access token (secret: never log it), refreshed first when it expires within kRefreshMargin. Concurrent
    // calls share one refresh. Fails with SignInRequired (no sign-in, or it expired: state Expired), NotConfigured
    // (Spotify rejected the Client ID), or a transient error (network, 5xx, rate limit) that leaves the sign-in in
    // place.
    QFuture<Result<QString>> accessToken();

    // A request with `token` got 401: the next accessToken() fetches a new one.
    void reportUnauthorized(const QString &token);

Q_SIGNALS:
    void stateChanged(selecta::meta::SpotifyAuth::State state);
    // The sign-in is gone (signOut(), expiry, Client ID change): drop Spotify data cached for it (Policy §I.2).
    void signedOut();
    // A sign-in worked but couldn't be saved: the user will have to sign in again after a restart.
    void persistFailed(const selecta::meta::MetaError &error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
