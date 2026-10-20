// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/TokenGenerator.h"

#include <QRandomGenerator>

#include <limits>

namespace selecta::slsk {

TokenGenerator::TokenGenerator()
    : TokenGenerator(randomStart())
{}

TokenGenerator::TokenGenerator(quint32 start)
    // next() returns start itself first (or 1 if start is 0).
    : m_last(start - 1)
{}

quint32 TokenGenerator::next()
{
    ++m_last; // unsigned: wraps from 2^32 - 1 to 0
    if (m_last == 0) {
        m_last = 1;
    }
    return m_last;
}

quint32 TokenGenerator::next(const std::function<bool(quint32)> &inUse)
{
    for (int probe = 0; probe < kMaxProbes; ++probe) {
        const quint32 token = next();
        if (!inUse || !inUse(token)) {
            return token;
        }
    }
    return 0;
}

quint32 TokenGenerator::randomStart()
{
    // [1, 2^32 / 1000], like Nicotine+: well away from the wrap, and different on every run.
    constexpr quint32 kHighest = std::numeric_limits<quint32>::max() / 1000;
    return QRandomGenerator::global()->bounded(kHighest) + 1;
}

} // namespace selecta::slsk
