// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtTypes>

#include <functional>

namespace selecta::slsk {

// Tokens for searches, transfers, folder requests and indirect connections (protocol §3.3): start at a random value,
// increment, wrap, and never produce 0. One generator per purpose.
//
// Not thread-safe; owned by one object on the engine thread.
class TokenGenerator
{
public:
    // Starts at a random value in [1, 2^32 / 1000], as Nicotine+ does.
    TokenGenerator();
    explicit TokenGenerator(quint32 start);

    // The next token after the previous one; 0 is skipped.
    quint32 next();
    // The next token for which `inUse` is false. Returns 0 if none is found within kMaxProbes tries (only possible
    // with about a million tokens in use at once).
    quint32 next(const std::function<bool(quint32)> &inUse);

    static constexpr int kMaxProbes = 1'048'576;

    static quint32 randomStart();

private:
    quint32 m_last;
};

} // namespace selecta::slsk
