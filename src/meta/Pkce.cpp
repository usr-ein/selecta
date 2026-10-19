// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/Pkce.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <algorithm>
#include <array>

namespace selecta::meta::pkce {
namespace {

constexpr QByteArrayView kUnreserved{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"};

bool isUnreserved(char c)
{
    return kUnreserved.contains(c);
}

} // namespace

QByteArray makeVerifier(int length)
{
    const int n = std::clamp(length, kMinVerifierLength, kMaxVerifierLength);
    QByteArray verifier(n, Qt::Uninitialized);
    QRandomGenerator *random = QRandomGenerator::system(); // the OS CSPRNG
    for (char &c : verifier) {
        c = kUnreserved.at(static_cast<qsizetype>(random->bounded(static_cast<quint32>(kUnreserved.size()))));
    }
    return verifier;
}

QByteArray challengeS256(QByteArrayView verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool isValidVerifier(QByteArrayView verifier)
{
    return verifier.size() >= kMinVerifierLength && verifier.size() <= kMaxVerifierLength &&
           std::ranges::all_of(verifier, isUnreserved);
}

QByteArray makeState()
{
    std::array<quint32, 4> bits{}; // 128 bits
    QRandomGenerator::system()->generate(bits.begin(), bits.end());
    const QByteArray raw(reinterpret_cast<const char *>(bits.data()), static_cast<qsizetype>(sizeof(bits)));
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool constantTimeEquals(QByteArrayView a, QByteArrayView b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        const auto x = static_cast<unsigned>(static_cast<unsigned char>(a[i]));
        const auto y = static_cast<unsigned>(static_cast<unsigned char>(b[i]));
        difference |= x ^ y;
    }
    return difference == 0U;
}

} // namespace selecta::meta::pkce
