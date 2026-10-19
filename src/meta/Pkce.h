// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QByteArrayView>

// Proof Key for Code Exchange (RFC 7636) with the S256 method, and the `state` value of the authorisation request.
// Pure functions; the random ones draw from the operating system's CSPRNG (QRandomGenerator::system()). Thread-safe.
namespace selecta::meta::pkce {

inline constexpr int kMinVerifierLength = 43; // RFC 7636 §4.1
inline constexpr int kMaxVerifierLength = 128;
inline constexpr int kDefaultVerifierLength = 64;

// A fresh code verifier: `length` characters (clamped to 43…128) from the unreserved set [A-Za-z0-9-._~].
QByteArray makeVerifier(int length = kDefaultVerifierLength);

// BASE64URL(SHA256(ASCII(verifier))) without padding (RFC 7636 §4.2, S256).
QByteArray challengeS256(QByteArrayView verifier);

// True when `verifier` is 43…128 unreserved characters, so the server accepts it.
bool isValidVerifier(QByteArrayView verifier);

// A fresh `state` for the authorisation request: 128 random bits, base64url without padding.
QByteArray makeState();

// Compares two secrets in time that depends only on their lengths, not their contents.
bool constantTimeEquals(QByteArrayView a, QByteArrayView b);

} // namespace selecta::meta::pkce
