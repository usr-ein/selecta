// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback/RingBuffer.h"

#include <gtest/gtest.h>
#include <numeric>
#include <thread>
#include <vector>

using selecta::playback::RingBuffer;

TEST(RingBufferTest, RoundsTheCapacityUpToAPowerOfTwo)
{
    EXPECT_EQ(RingBuffer(1).capacity(), 1U);
    EXPECT_EQ(RingBuffer(5).capacity(), 8U);
    EXPECT_EQ(RingBuffer(1024).capacity(), 1024U);
}

TEST(RingBufferTest, WritesWhatFitsAndReadsItBackInOrder)
{
    RingBuffer ring(8);
    std::vector<float> input(12);
    std::iota(input.begin(), input.end(), 1.0F);
    EXPECT_EQ(ring.write(input.data(), input.size()), 8U); // the rest doesn't fit
    EXPECT_EQ(ring.freeSpace(), 0U);
    EXPECT_EQ(ring.available(), 8U);

    std::vector<float> output(5);
    EXPECT_EQ(ring.read(output.data(), output.size()), 5U);
    EXPECT_EQ(output, (std::vector<float>{1, 2, 3, 4, 5}));
    EXPECT_EQ(ring.readPosition(), 5U);

    // Wraps around the end of the storage.
    EXPECT_EQ(ring.write(input.data() + 8, 4), 4U);
    output.assign(7, 0.0F);
    EXPECT_EQ(ring.read(output.data(), output.size()), 7U);
    EXPECT_EQ(output, (std::vector<float>{6, 7, 8, 9, 10, 11, 12}));
    EXPECT_EQ(ring.available(), 0U);
    EXPECT_EQ(ring.read(output.data(), 1), 0U);
}

TEST(RingBufferTest, SkipsToAPositionButNeverPastTheWrittenPart)
{
    RingBuffer ring(16);
    const std::vector<float> input(10, 1.0F);
    ring.write(input.data(), input.size());
    EXPECT_EQ(ring.writePosition(), 10U);

    ring.skipTo(4);
    EXPECT_EQ(ring.readPosition(), 4U);
    ring.skipTo(2); // never backwards
    EXPECT_EQ(ring.readPosition(), 4U);
    ring.skipTo(SIZE_MAX); // "everything written so far"
    EXPECT_EQ(ring.readPosition(), 10U);
    EXPECT_EQ(ring.available(), 0U);
    EXPECT_EQ(ring.freeSpace(), 16U);
}

TEST(RingBufferTest, CarriesEverySampleAcrossThreadsInOrder)
{
    RingBuffer ring(256);
    constexpr std::size_t kTotal = 200'000;
    std::thread producer([&ring] {
        std::size_t next = 0;
        std::vector<float> chunk(37);
        while (next < kTotal) {
            const std::size_t count = std::min(chunk.size(), kTotal - next);
            for (std::size_t i = 0; i < count; ++i) {
                chunk[i] = static_cast<float>((next + i) % 1'000'000);
            }
            next += ring.write(chunk.data(), count);
        }
    });
    std::size_t expected = 0;
    std::size_t mismatches = 0;
    std::vector<float> chunk(53);
    while (expected < kTotal) {
        const std::size_t got = ring.read(chunk.data(), chunk.size());
        for (std::size_t i = 0; i < got; ++i) {
            if (chunk[i] != static_cast<float>((expected + i) % 1'000'000)) {
                ++mismatches;
            }
        }
        expected += got;
    }
    producer.join();
    EXPECT_EQ(mismatches, 0U);
    EXPECT_EQ(ring.readPosition(), kTotal);
}
