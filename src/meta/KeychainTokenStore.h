// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/TokenStore.h"

#include <QString>
#include <QThreadPool>

namespace selecta::meta {

// TokenStore over the macOS login Keychain: one generic-password item per account, under the Keychain service name
// given here (Security framework, SecItem*). The only dependency meta has beyond Qt (contract: meta.md).
//
// Every Keychain call runs on a private one-thread pool, in call order, because SecItem calls can block on a
// system prompt. Destroying the store drops calls that haven't started and waits for the one running, if any.
//
// On platforms without the Keychain every call fails with ErrorCode::NotConfigured.
class KeychainTokenStore final : public TokenStore
{
public:
    // `keychainService` is the kSecAttrService of the items ("app.selecta.Selecta" in the app; a unique name in a
    // manual check, so it never touches the app's items).
    explicit KeychainTokenStore(QString keychainService);
    ~KeychainTokenStore() override;
    KeychainTokenStore(const KeychainTokenStore &) = delete;
    KeychainTokenStore &operator=(const KeychainTokenStore &) = delete;
    KeychainTokenStore(KeychainTokenStore &&) = delete;
    KeychainTokenStore &operator=(KeychainTokenStore &&) = delete;

    QFuture<Result<std::optional<QByteArray>>> read(const QString &account) override;
    QFuture<Result<void>> write(const QString &account, const QByteArray &secret) override;
    QFuture<Result<void>> remove(const QString &account) override;

private:
    const QString m_service;
    QThreadPool m_pool;
};

} // namespace selecta::meta
