// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace selecta::playback {

// A single-producer, single-consumer ring of floats, wait-free on both sides: the decoder thread writes, the audio
// thread reads. No locks and no allocation after construction, so the real-time audio callback may use it.
//
// Positions are absolute counts of floats written (head) and read (tail) since construction; they only grow (a 64-bit
// count doesn't wrap in practice), so a position names one sample for good. That is what lets the consumer drop stale
// audio up to a position the producer marked (skipTo()) without either side waiting for the other.
//
// Threading: write(), freeSpace() and writePosition() from the producer thread only; read(), available() and skipTo()
// from the consumer thread only. readPosition() from any thread.
class RingBuffer
{
public:
    // Capacity is rounded up to a power of two.
    explicit RingBuffer(std::size_t capacity)
        : m_capacity(roundUp(capacity))
        , m_mask(m_capacity - 1)
        , m_data(m_capacity)
    {}

    std::size_t capacity() const { return m_capacity; }

    // Producer: copies up to `count` floats in; returns how many fitted.
    std::size_t write(const float *source, std::size_t count)
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        const std::size_t n = std::min(count, m_capacity - (head - tail));
        for (std::size_t i = 0; i < n; ++i) {
            m_data[(head + i) & m_mask] = source[i];
        }
        m_head.store(head + n, std::memory_order_release);
        return n;
    }

    // Producer: room for this many floats.
    std::size_t freeSpace() const
    {
        return m_capacity - (m_head.load(std::memory_order_relaxed) - m_tail.load(std::memory_order_acquire));
    }

    // Producer: the position the next write() starts at.
    std::size_t writePosition() const { return m_head.load(std::memory_order_relaxed); }

    // Consumer: copies up to `count` floats out; returns how many there were.
    std::size_t read(float *destination, std::size_t count)
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t n = std::min(count, m_head.load(std::memory_order_acquire) - tail);
        for (std::size_t i = 0; i < n; ++i) {
            destination[i] = m_data[(tail + i) & m_mask];
        }
        m_tail.store(tail + n, std::memory_order_release);
        return n;
    }

    // Consumer: floats ready to read.
    std::size_t available() const
    {
        return m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_relaxed);
    }

    // Consumer: drops everything before `position` (never past what has been written).
    void skipTo(std::size_t position)
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t target = std::min(position, m_head.load(std::memory_order_acquire));
        if (target > tail) {
            m_tail.store(target, std::memory_order_release);
        }
    }

    // Any thread: how far the consumer has read. Only grows.
    std::size_t readPosition() const { return m_tail.load(std::memory_order_acquire); }

private:
    static std::size_t roundUp(std::size_t value)
    {
        std::size_t capacity = 1;
        while (capacity < value) {
            capacity <<= 1U;
        }
        return capacity;
    }

    // The two counters sit on separate cache lines, so each side's writes don't slow the other's reads.
    alignas(64) std::atomic<std::size_t> m_head{0}; // written by the producer
    const std::size_t m_capacity;
    const std::size_t m_mask;
    std::vector<float> m_data;
    alignas(64) std::atomic<std::size_t> m_tail{0}; // written by the consumer
};

} // namespace selecta::playback
