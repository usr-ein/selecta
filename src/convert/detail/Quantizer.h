// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#include <span>

namespace selecta::convert::detail {

// Float samples in [-1, 1] to integers of 16 or 24 bits (conversion §2.2), with TPDF dither of ±1 LSB or plain
// rounding. A sample already on the target grid comes back exactly (k / 2^(bits-1) → k) with or without dither: that
// makes a 16- or 24-bit source bit-identical after conversion, keeps digital silence silent, and reduces padded 24-bit
// audio to 16 bits losslessly. Values beyond full scale are clipped and counted. The dither noise is deterministic
// (fixed seed): the same input gives the same output.
//
// Threading: one thread at a time.
class Quantizer
{
public:
    Quantizer(int bits, bool dither);

    // Writes min(in.size(), out.size()) samples, each in [-2^(bits-1), 2^(bits-1) - 1].
    void quantize(std::span<const float> in, std::span<qint32> out);

    int bits() const { return m_bits; }
    qint64 clippedSamples() const { return m_clipped; }

private:
    double uniform(); // [0, 1)

    int m_bits = 16;
    bool m_dither = false;
    double m_scale = 32768.0;
    double m_min = -32768.0;
    double m_max = 32767.0;
    quint64 m_state = 0x9E3779B97F4A7C15ULL;
    qint64 m_clipped = 0;
};

} // namespace selecta::convert::detail
