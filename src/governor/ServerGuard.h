// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHostAddress>
#include <QString>

// Keeps development builds, tests and tools off the live Soulseek network (PRD SAFE-3, CLAUDE.md).
//
// A server is local when its address is loopback, private (RFC 1918, unique-local) or link-local. Anything else is the
// live network, including carrier-grade NAT ranges. Connecting to the live network needs both a build with
// SELECTA_ENABLE_LIVE_SERVER and the user's explicit choice at run time. Host names other than `localhost` must be
// resolved first and every resolved address checked. All functions are pure and thread-safe.
namespace selecta::governor {

enum class ServerKind {
    Local,      // a test server on this machine or the local network
    Live,       // the public Soulseek network
    Unresolved, // a host name: resolve it, then check each address with addressAllowed()
    Invalid,    // empty, unspecified (0.0.0.0, ::), multicast or broadcast: never a server
};

ServerKind classifyServerHost(const QString &host);
ServerKind classifyAddress(const QHostAddress &address);

// True for the official server's names, whatever they resolve to. Case and a trailing dot don't matter.
bool isKnownLiveServerName(const QString &host);

// True when this build may ever reach the live network (CMake option SELECTA_ENABLE_LIVE_SERVER).
bool liveServerCompiledIn();

// Whether connecting is allowed now. Unresolved names and invalid addresses are always refused.
bool serverAllowed(const QString &host, bool liveEnabledByUser);
bool addressAllowed(const QHostAddress &address, bool liveEnabledByUser);

} // namespace selecta::governor
