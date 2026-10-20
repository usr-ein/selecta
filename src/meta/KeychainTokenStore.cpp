// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/KeychainTokenStore.h"

#include "meta/detail/Async.h"

#include <QtGlobal>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::meta {
namespace {

#ifdef Q_OS_MACOS

// Owns one Core Foundation reference.
template <typename T>
class CfRef
{
public:
    CfRef() = default;
    explicit CfRef(T ref)
        : m_ref(ref)
    {}
    ~CfRef()
    {
        if (m_ref != nullptr) {
            CFRelease(m_ref);
        }
    }
    CfRef(const CfRef &) = delete;
    CfRef &operator=(const CfRef &) = delete;
    CfRef(CfRef &&other) noexcept
        : m_ref(std::exchange(other.m_ref, nullptr))
    {}
    CfRef &operator=(CfRef &&other) noexcept
    {
        std::swap(m_ref, other.m_ref);
        return *this;
    }

    T get() const { return m_ref; }
    T *out() { return &m_ref; }

private:
    T m_ref = nullptr;
};

QString statusText(OSStatus status)
{
    const CfRef<CFStringRef> text(SecCopyErrorMessageString(status, nullptr));
    const QString message = text.get() != nullptr ? QString::fromCFString(text.get()) : QString();
    return u"Keychain error %1%2"_s.arg(status).arg(message.isEmpty() ? QString() : u": "_s + message);
}

MetaError keychainError(OSStatus status)
{
    // errSecAuthFailed / errSecUserCanceled: the user declined the system prompt.
    const ErrorCode code =
        (status == errSecUserCanceled || status == errSecAuthFailed) ? ErrorCode::AccessDenied : ErrorCode::Internal;
    return makeError(Service::Keychain, code, statusText(status));
}

// The attributes that identify one item: class, service and account.
CfRef<CFMutableDictionaryRef> itemQuery(const QString &service, const QString &account)
{
    CfRef<CFMutableDictionaryRef> query(
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const CfRef<CFStringRef> serviceRef(service.toCFString());
    const CfRef<CFStringRef> accountRef(account.toCFString());
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, serviceRef.get());
    CFDictionarySetValue(query.get(), kSecAttrAccount, accountRef.get());
    return query;
}

Result<std::optional<QByteArray>> readItem(const QString &service, const QString &account)
{
    const CfRef<CFMutableDictionaryRef> query = itemQuery(service, account);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    CfRef<CFTypeRef> data;
    const OSStatus status = SecItemCopyMatching(query.get(), data.out());
    if (status == errSecItemNotFound) {
        return std::optional<QByteArray>();
    }
    if (status != errSecSuccess) {
        return keychainError(status);
    }
    if (data.get() == nullptr || CFGetTypeID(data.get()) != CFDataGetTypeID()) {
        return makeError(Service::Keychain, ErrorCode::BadResponse, u"The Keychain item holds no data."_s);
    }
    return std::optional<QByteArray>(QByteArray::fromCFData(static_cast<CFDataRef>(data.get())));
}

Result<void> writeItem(const QString &service, const QString &account, const QByteArray &secret)
{
    const CfRef<CFMutableDictionaryRef> query = itemQuery(service, account);
    const CfRef<CFDataRef> value(secret.toCFData());
    const CfRef<CFMutableDictionaryRef> update(
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(update.get(), kSecValueData, value.get());
    OSStatus status = SecItemUpdate(query.get(), update.get());
    if (status == errSecItemNotFound) {
        const CfRef<CFStringRef> label(QString(u"Selecta: "_s + account).toCFString());
        CFDictionarySetValue(query.get(), kSecValueData, value.get());
        CFDictionarySetValue(query.get(), kSecAttrLabel, label.get());
        status = SecItemAdd(query.get(), nullptr);
    }
    if (status != errSecSuccess) {
        return keychainError(status);
    }
    return {};
}

Result<void> removeItem(const QString &service, const QString &account)
{
    const CfRef<CFMutableDictionaryRef> query = itemQuery(service, account);
    const OSStatus status = SecItemDelete(query.get());
    if (status != errSecSuccess && status != errSecItemNotFound) {
        return keychainError(status);
    }
    return {};
}

#else

MetaError unsupported()
{
    return makeError(Service::Keychain, ErrorCode::NotConfigured, u"The Keychain exists only on macOS."_s);
}

Result<std::optional<QByteArray>> readItem(const QString &, const QString &)
{
    return unsupported();
}
Result<void> writeItem(const QString &, const QString &, const QByteArray &)
{
    return unsupported();
}
Result<void> removeItem(const QString &, const QString &)
{
    return unsupported();
}

#endif

// Runs `work` on `pool` and finishes the future with its result (on the pool's thread).
template <typename T, typename Work>
QFuture<T> runOn(QThreadPool &pool, Work work)
{
    const detail::SharedPromise<T> promise;
    pool.start([promise, work = std::move(work)]() mutable { promise.finish(work()); });
    return promise.future();
}

} // namespace

KeychainTokenStore::KeychainTokenStore(QString keychainService)
    : m_service(std::move(keychainService))
{
    m_pool.setMaxThreadCount(1); // calls run one at a time, in order
    m_pool.setObjectName(u"selecta-keychain"_s);
}

KeychainTokenStore::~KeychainTokenStore()
{
    m_pool.clear();       // calls not started yet: their futures end canceled
    m_pool.waitForDone(); // the one running (if any) finishes
}

QFuture<Result<std::optional<QByteArray>>> KeychainTokenStore::read(const QString &account)
{
    return runOn<Result<std::optional<QByteArray>>>(
        m_pool, [service = m_service, account] { return readItem(service, account); });
}

QFuture<Result<void>> KeychainTokenStore::write(const QString &account, const QByteArray &secret)
{
    return runOn<Result<void>>(m_pool,
                               [service = m_service, account, secret] { return writeItem(service, account, secret); });
}

QFuture<Result<void>> KeychainTokenStore::remove(const QString &account)
{
    return runOn<Result<void>>(m_pool, [service = m_service, account] { return removeItem(service, account); });
}

} // namespace selecta::meta
