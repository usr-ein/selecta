// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/detail/Quantizer.h"

#include <algorithm>
#include <cmath>

namespace selecta::convert::detail {

Quantizer::Quantizer(int bits, bool dither)
    : m_bits(std::clamp(bits, 8, 24))
    , m_dither(dither)
    , m_scale(std::ldexp(1.0, m_bits - 1))
    , m_min(-m_scale)
    , m_max(m_scale - 1.0)
{}

double Quantizer::uniform()
{
    // xorshift64*: fast, and plenty for dither noise.
    m_state ^= m_state >> 12U;
    m_state ^= m_state << 25U;
    m_state ^= m_state >> 27U;
    const quint64 value = m_state * 0x2545F4914F6CDD1DULL;
    return std::ldexp(static_cast<double>(value >> 11U), -53);
}

void Quantizer::quantize(std::span<const float> in, std::span<qint32> out)
{
    const std::size_t count = std::min(in.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        double value = static_cast<double>(in[i]) * m_scale;
        if (m_dither) {
            value += uniform() - uniform(); // triangular, ±1 LSB
        }
        if (!(value > m_min)) { // also catches NaN
            value = m_min;
            m_clipped += in[i] < -1.0F ? 1 : 0;
        } else if (value > m_max) {
            value = m_max;
            m_clipped += in[i] >= 1.0F ? 1 : 0;
        }
        out[i] = static_cast<qint32>(std::lrint(value));
    }
}

} // namespace selecta::convert::detail
