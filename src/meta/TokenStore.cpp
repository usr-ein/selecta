// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/TokenStore.h"

#include "meta/detail/Async.h"

#include <QMutexLocker>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace selecta::meta {

TokenStore::~TokenStore() = default;

namespace {

// Finishes on the next turn of the calling thread's event loop, like a store with a worker thread would.
template <typename T>
QFuture<T> later(T value)
{
    const detail::SharedPromise<T> promise;
    QTimer::singleShot(0, [promise, value = std::move(value)]() mutable { promise.finish(std::move(value)); });
    return promise.future();
}

MetaError storeError()
{
    return makeError(Service::Keychain, ErrorCode::Internal, u"The secret store is failing (test setting)."_s);
}

} // namespace

QFuture<Result<std::optional<QByteArray>>> InMemoryTokenStore::read(const QString &account)
{
    const QMutexLocker lock(&m_mutex);
    if (m_failing) {
        return later(Result<std::optional<QByteArray>>(storeError()));
    }
    const auto found = m_secrets.constFind(account);
    std::optional<QByteArray> secret;
    if (found != m_secrets.constEnd()) {
        secret = found.value();
    }
    return later(Result<std::optional<QByteArray>>(std::move(secret)));
}

QFuture<Result<void>> InMemoryTokenStore::write(const QString &account, const QByteArray &secret)
{
    const QMutexLocker lock(&m_mutex);
    if (m_failing) {
        return later(Result<void>(storeError()));
    }
    m_secrets.insert(account, secret);
    return later(Result<void>());
}

QFuture<Result<void>> InMemoryTokenStore::remove(const QString &account)
{
    const QMutexLocker lock(&m_mutex);
    if (m_failing) {
        return later(Result<void>(storeError()));
    }
    m_secrets.remove(account);
    return later(Result<void>());
}

void InMemoryTokenStore::setFailing(bool failing)
{
    const QMutexLocker lock(&m_mutex);
    m_failing = failing;
}

std::optional<QByteArray> InMemoryTokenStore::peek(const QString &account) const
{
    const QMutexLocker lock(&m_mutex);
    const auto found = m_secrets.constFind(account);
    if (found == m_secrets.constEnd()) {
        return std::nullopt;
    }
    return found.value();
}

} // namespace selecta::meta
