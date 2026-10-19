// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/ServerGuard.h"

using namespace Qt::StringLiterals;

namespace selecta::governor {
namespace {

// Lower case, without surrounding spaces or the trailing dot of a fully qualified name.
QString normalizedName(const QString &host)
{
    QString name = host.trimmed().toLower();
    while (name.endsWith(u'.'))
        name.chop(1);
    return name;
}

bool isAllowed(ServerKind kind, bool liveEnabledByUser)
{
    switch (kind) {
    case ServerKind::Local:
        return true;
    case ServerKind::Live:
        return liveServerCompiledIn() && liveEnabledByUser;
    case ServerKind::Unresolved:
    case ServerKind::Invalid:
        return false;
    }
    return false;
}

} // namespace

bool isKnownLiveServerName(const QString &host)
{
    const QString name = normalizedName(host);
    return name == u"slsknet.org"_s || name.endsWith(u".slsknet.org"_s);
}

ServerKind classifyAddress(const QHostAddress &address)
{
    if (address.isNull())
        return ServerKind::Unresolved;

    // An IPv4-mapped IPv6 address (::ffff:a.b.c.d) is classified as the IPv4 address it carries.
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    const QHostAddress normalized = isV4 ? QHostAddress(v4) : address;

    if (normalized == QHostAddress(QHostAddress::AnyIPv4) || normalized == QHostAddress(QHostAddress::AnyIPv6) ||
        normalized.isMulticast() || normalized.isBroadcast())
        return ServerKind::Invalid;
    if (normalized.isLoopback() || normalized.isPrivateUse() || normalized.isLinkLocal())
        return ServerKind::Local;
    return ServerKind::Live;
}

ServerKind classifyServerHost(const QString &host)
{
    const QString name = normalizedName(host);
    if (name.isEmpty())
        return ServerKind::Invalid;
    if (isKnownLiveServerName(name))
        return ServerKind::Live;
    if (name == u"localhost"_s)
        return ServerKind::Local;
    const QHostAddress address(name);
    return address.isNull() ? ServerKind::Unresolved : classifyAddress(address);
}

bool liveServerCompiledIn()
{
#ifdef SELECTA_LIVE_SERVER_COMPILED_IN
    return true;
#else
    return false;
#endif
}

bool serverAllowed(const QString &host, bool liveEnabledByUser)
{
    return isAllowed(classifyServerHost(host), liveEnabledByUser);
}

bool addressAllowed(const QHostAddress &address, bool liveEnabledByUser)
{
    return isAllowed(classifyAddress(address), liveEnabledByUser);
}

} // namespace selecta::governor
