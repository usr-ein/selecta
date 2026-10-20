// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Result.h"

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QString>

#include <optional>

namespace selecta::meta {

// Where secrets live: the Spotify refresh token (and, later, any other credential). Production uses the macOS
// Keychain (KeychainTokenStore.h); tests and throwaway runs use InMemoryTokenStore. Secrets never go to settings
// files, logs or the repository (META-1, D21).
//
// Every call is asynchronous because the Keychain can block on a system prompt (an ad-hoc signed development build
// asks again after each rebuild). The returned future finishes on an unspecified thread: attach continuations with a
// context object (`future.then(this, …)`) to get back to the caller's thread. Errors use Service::Keychain.
//
// Ownership: the store must outlive the calls made on it. Thread-safe: calls may come from any thread; calls are
// carried out in the order they were made.
class TokenStore
{
public:
    TokenStore() = default;
    TokenStore(const TokenStore &) = delete;
    TokenStore &operator=(const TokenStore &) = delete;
    TokenStore(TokenStore &&) = delete;
    TokenStore &operator=(TokenStore &&) = delete;
    virtual ~TokenStore();

    // The secret stored for `account`, or nullopt when there is none.
    virtual QFuture<Result<std::optional<QByteArray>>> read(const QString &account) = 0;
    // Creates or replaces the secret for `account`.
    virtual QFuture<Result<void>> write(const QString &account, const QByteArray &secret) = 0;
    // Deletes the secret for `account`; succeeds when there was none.
    virtual QFuture<Result<void>> remove(const QString &account) = 0;
};

// Secrets in memory only, gone when the object is. Futures finish on the next turn of the calling thread's event loop
// (never inside the call), like a real store would. setFailing() makes every call fail, for error-path tests.
class InMemoryTokenStore final : public TokenStore
{
public:
    InMemoryTokenStore() = default;

    QFuture<Result<std::optional<QByteArray>>> read(const QString &account) override;
    QFuture<Result<void>> write(const QString &account, const QByteArray &secret) override;
    QFuture<Result<void>> remove(const QString &account) override;

    void setFailing(bool failing);
    // Direct access for tests: the secret stored now, without going through a future.
    std::optional<QByteArray> peek(const QString &account) const;

private:
    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_secrets;
    bool m_failing = false;
};

} // namespace selecta::meta
