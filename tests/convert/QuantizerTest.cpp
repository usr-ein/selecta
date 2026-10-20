// SPDX-License-Identifier: GPL-3.0-or-later
// C2: float → integer quantisation: exact for audio already on the grid, TPDF dither of ±1 LSB otherwise.
#include "convert/detail/Quantizer.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

using selecta::convert::detail::Quantizer;

TEST(Quantizer, SamplesOnTheGridComeBackExactly)
{
    for (const int bits : {16, 24}) {
        const double scale = std::ldexp(1.0, bits - 1);
        std::vector<float> in;
        std::vector<qint32> expected;
        for (const qint32 k : {-static_cast<qint32>(scale), -12345, -1, 0, 1, 777, static_cast<qint32>(scale) - 1}) {
            in.push_back(static_cast<float>(k / scale));
            expected.push_back(k);
        }
        std::vector<qint32> out(in.size());
        Quantizer quantizer(bits, /*dither=*/false);
        quantizer.quantize(in, out);
        EXPECT_EQ(out, expected) << bits;
        EXPECT_EQ(quantizer.clippedSamples(), 0);
    }
}

TEST(Quantizer, TpdfDitherStaysWithinOneLsbAndAveragesOut)
{
    // A value halfway between two 16-bit steps: undithered it always rounds the same way; dithered it lands on either
    // neighbour, half the time each, so the average stays at the true value.
    constexpr int kCount = 200000;
    const auto value = static_cast<float>(100.5 / 32768.0);
    const std::vector<float> in(kCount, value);
    std::vector<qint32> out(kCount);
    Quantizer quantizer(16, /*dither=*/true);
    quantizer.quantize(in, out);
    double sum = 0.0;
    for (const qint32 sample : out) {
        EXPECT_GE(sample, 99);
        EXPECT_LE(sample, 102);
        sum += sample;
    }
    EXPECT_NEAR(sum / kCount, 100.5, 0.02);

    // Deterministic: a second quantiser makes the same noise.
    std::vector<qint32> again(kCount);
    Quantizer(16, true).quantize(in, again);
    EXPECT_EQ(out, again);
}

TEST(Quantizer, DitherLeavesSamplesOnTheGridAlone)
{
    // Padded 16-bit audio and digital silence reduce to 16 bits exactly, dither on.
    std::vector<float> in;
    std::vector<qint32> expected;
    for (const qint32 k : {0, 0, 1, -1, 12345, -32768, 32767, 0}) {
        in.push_back(static_cast<float>(k / 32768.0));
        expected.push_back(k);
    }
    std::vector<qint32> out(in.size());
    Quantizer(16, /*dither=*/true).quantize(in, out);
    EXPECT_EQ(out, expected);
}

TEST(Quantizer, ClipsAndCountsBeyondFullScale)
{
    const std::vector<float> in = {1.0F, 1.5F, -1.0F, -2.0F, std::numeric_limits<float>::quiet_NaN(), 0.999999F};
    std::vector<qint32> out(in.size());
    Quantizer quantizer(16, false);
    quantizer.quantize(in, out);
    EXPECT_EQ(out[0], 32767);
    EXPECT_EQ(out[1], 32767);
    EXPECT_EQ(out[2], -32768); // exactly full scale: not a clip
    EXPECT_EQ(out[3], -32768);
    EXPECT_EQ(out[4], -32768); // NaN never reaches the integer conversion
    EXPECT_EQ(out[5], 32767);  // rounds up to 32768, which doesn't exist
    EXPECT_EQ(quantizer.clippedSamples(), 3);

    Quantizer deep(24, false);
    std::vector<qint32> deepOut(2);
    deep.quantize(std::vector<float>{1.0F, -1.0F}, deepOut);
    EXPECT_EQ(deepOut[0], 8388607);
    EXPECT_EQ(deepOut[1], -8388608);
}

TEST(Quantizer, WritesOnlyWhatFits)
{
    const std::vector<float> in = {0.5F, 0.5F, 0.5F};
    std::vector<qint32> out = {7, 7};
    Quantizer(16, false).quantize(in, out);
    EXPECT_EQ(out, (std::vector<qint32>{16384, 16384}));
}
