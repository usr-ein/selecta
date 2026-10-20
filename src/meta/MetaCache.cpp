// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/MetaCache.h"

namespace selecta::meta {

MetaCache::~MetaCache() = default;

MemoryMetaCache::MemoryMetaCache(const base::Clock &clock, qint64 maxBytes)
    : m_clock(clock)
    , m_maxBytes(std::max<qint64>(0, maxBytes))
{}

qint64 MemoryMetaCache::cost(const Entry &entry)
{
    // UTF-16 key plus value, plus a rough per-entry overhead so a flood of tiny entries is bounded too.
    constexpr qint64 kOverhead = 64;
    return (qint64{entry.key.size()} * 2) + qint64{entry.value.size()} + kOverhead;
}

void MemoryMetaCache::erase(Lru::iterator it)
{
    m_bytes -= cost(*it);
    m_index.remove(it->key);
    m_lru.erase(it);
}

std::optional<QByteArray> MemoryMetaCache::get(const QString &key)
{
    const auto found = m_index.constFind(key);
    if (found == m_index.constEnd()) {
        return std::nullopt;
    }
    const auto it = found.value();
    if (m_clock.now() >= it->expires) {
        erase(it);
        return std::nullopt;
    }
    m_lru.splice(m_lru.begin(), m_lru, it); // most recently used first; iterators stay valid
    return it->value;
}

void MemoryMetaCache::put(const QString &key, const QByteArray &value, std::chrono::seconds ttl)
{
    if (const auto found = m_index.constFind(key); found != m_index.constEnd()) {
        erase(found.value());
    }
    if (ttl <= std::chrono::seconds::zero()) {
        return;
    }
    Entry entry{key, value, m_clock.now() + std::chrono::duration_cast<base::Clock::Duration>(ttl)};
    const qint64 entryCost = cost(entry);
    if (entryCost > m_maxBytes) {
        return; // would evict everything and still not fit
    }
    while (!m_lru.empty() && m_bytes + entryCost > m_maxBytes) {
        erase(std::prev(m_lru.end()));
    }
    m_lru.push_front(std::move(entry));
    m_index.insert(key, m_lru.begin());
    m_bytes += entryCost;
}

void MemoryMetaCache::removePrefix(const QString &prefix)
{
    for (auto it = m_lru.begin(); it != m_lru.end();) {
        const auto next = std::next(it);
        if (it->key.startsWith(prefix)) {
            erase(it);
        }
        it = next;
    }
}

} // namespace selecta::meta
