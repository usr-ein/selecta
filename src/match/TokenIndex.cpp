// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/TokenIndex.h"

#include "match/Lexicon.h"

#include <QString>
#include <QVarLengthArray>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <utility>
#include <vector>

namespace selecta::match {

namespace detail {

struct TokenIndexData
{
    // (hash, term) pairs sorted by hash, with a directory on the hash's top bits: a lookup searches one small
    // bucket. Hashes keep 32 bits: a collision only adds a candidate, and every candidate is verified.
    struct HashedTerms
    {
        std::vector<quint32> hashes;    // sorted
        std::vector<quint32> terms;     // terms[i] belongs to hashes[i]
        std::vector<quint8> lengths;    // the UTF-16 length of terms[i], at most 255: filters before any lookup
        std::vector<quint32> directory; // bucket b holds entries [directory[b], directory[b + 1])
        int shift = 31;                 // bucket of a hash: hash >> shift
    };

    std::vector<char16_t> pool;          // term texts back to back
    std::vector<quint32> termOffsets{0}; // term t is pool[termOffsets[t], termOffsets[t + 1])
    std::vector<quint32> buckets;        // open addressing on the text hash: term ids (kNoTerm = free), load ≤ 1/2
    std::vector<std::pair<quint32, quint32>> pending; // (term, document) while building

    std::vector<quint32> offsets; // postings of term t: documents[offsets[t], offsets[t + 1])
    std::vector<quint32> documents;
    HashedTerms deletes; // symmetric deletes of the first kDeletePrefix characters (§17.6)
    HashedTerms leet;    // leet-folded forms of terms that mix letters and digits
    bool fuzzy = true;
};

} // namespace detail

namespace {

using detail::TokenIndexData;
using HashedTerms = TokenIndexData::HashedTerms;

constexpr quint64 kFnvOffset = 0xCBF29CE484222325ULL;
constexpr quint64 kFnvPrime = 0x100000001B3ULL;
constexpr qsizetype kMaxDeletes = 29; // 1 + 7 + 21: two deletes from a 7-character prefix
constexpr int kMaxDirectoryBits = 20;
constexpr std::size_t kEntriesPerBucket = 16;

// FNV-1a over UTF-16 code units.
quint64 hashOf(QStringView text)
{
    quint64 hash = kFnvOffset;
    for (const QChar c : text) {
        hash ^= static_cast<quint64>(c.unicode());
        hash *= kFnvPrime;
    }
    return hash;
}

// The 32 bits kept by HashedTerms: FNV-1a's high half mixes best.
quint32 shortHash(quint64 hash)
{
    return static_cast<quint32>(hash >> 32U);
}

// Slot of a text hash in the vocabulary table (a finaliser, so the low bits mix well).
std::size_t slotOf(quint64 hash, std::size_t mask)
{
    hash ^= hash >> 32U;
    hash *= 0xD6E8FEB86659FD93ULL;
    hash ^= hash >> 32U;
    return static_cast<std::size_t>(hash) & mask;
}

QStringView textOf(const TokenIndexData &data, quint32 term)
{
    const quint32 begin = data.termOffsets[term];
    return {data.pool.data() + begin, static_cast<qsizetype>(data.termOffsets[term + 1] - begin)};
}

// The slot holding `text`, or the free slot where it belongs. The table is never full (load ≤ 1/2).
std::size_t probe(const TokenIndexData &data, QStringView text, quint64 hash)
{
    const std::size_t mask = data.buckets.size() - 1;
    for (std::size_t slot = slotOf(hash, mask);; slot = (slot + 1) & mask) {
        const quint32 term = data.buckets[slot];
        if (term == TokenIndex::kNoTerm || textOf(data, term) == text) {
            return slot;
        }
    }
}

void grow(TokenIndexData &data)
{
    data.buckets.assign(std::max<std::size_t>(16, data.buckets.size() * 2), TokenIndex::kNoTerm);
    const auto count = static_cast<quint32>(data.termOffsets.size() - 1);
    for (quint32 term = 0; term < count; ++term) {
        const QStringView text = textOf(data, term);
        data.buckets[probe(data, text, hashOf(text))] = term;
    }
}

bool hasDigit(QStringView text)
{
    return std::ranges::any_of(text, [](QChar c) { return c.isDigit(); });
}

bool hasLetter(QStringView text)
{
    return std::ranges::any_of(text, [](QChar c) { return c.isLetter(); });
}

// hashOf(TokenRules::leetFold(text)), without building the folded string; `changed` tells whether folding changes it.
quint64 leetHash(QStringView text, bool &changed)
{
    const bool fold = hasDigit(text) && hasLetter(text);
    changed = false;
    quint64 hash = kFnvOffset;
    for (const QChar c : text) {
        char16_t unit = c.unicode();
        if (fold && c.isDigit()) {
            if (const char16_t replacement = detail::lexicon::leetFold(unit); replacement != 0) {
                unit = replacement;
                changed = true;
            }
        }
        hash ^= static_cast<quint64>(unit);
        hash *= kFnvPrime;
    }
    return hash;
}

// The hashes of every string made by deleting up to `budget` (≤ 2) code units from the first kDeletePrefix units of
// `text`, the prefix itself included; sorted, without duplicates. No string is built.
struct DeleteHashes
{
    std::array<quint32, kMaxDeletes> values{};
    qsizetype count = 0;
};

DeleteHashes deleteHashes(QStringView text, int budget)
{
    const QStringView prefix = text.first(std::min(text.size(), TokenIndex::kDeletePrefix));
    const qsizetype n = prefix.size();
    const auto skipping = [prefix, n](qsizetype skipA, qsizetype skipB) {
        quint64 hash = kFnvOffset;
        for (qsizetype k = 0; k < n; ++k) {
            if (k != skipA && k != skipB) {
                hash ^= static_cast<quint64>(prefix[k].unicode());
                hash *= kFnvPrime;
            }
        }
        return shortHash(hash);
    };
    DeleteHashes out;
    out.values[out.count++] = skipping(-1, -1);
    for (qsizetype i = 0; budget >= 1 && i < n; ++i) {
        out.values[out.count++] = skipping(i, -1);
        for (qsizetype j = i + 1; budget >= 2 && j < n; ++j) {
            out.values[out.count++] = skipping(i, j);
        }
    }
    const auto values = std::span(out.values).first(static_cast<std::size_t>(out.count));
    std::ranges::sort(values);
    out.count = std::ranges::unique(values).begin() - values.begin();
    return out;
}

constexpr int kLongTerm = 255;

quint8 storedLength(qsizetype length)
{
    return static_cast<quint8>(std::min<qsizetype>(length, kLongTerm));
}

HashedTerms hashedTerms(std::vector<std::pair<quint32, quint32>> pairs, const TokenIndexData &data)
{
    std::ranges::sort(pairs);
    const auto [first, last] = std::ranges::unique(pairs);
    pairs.erase(first, last);
    HashedTerms table;
    table.hashes.reserve(pairs.size());
    table.terms.reserve(pairs.size());
    table.lengths.reserve(pairs.size());
    for (const auto &[hash, term] : pairs) {
        table.hashes.push_back(hash);
        table.terms.push_back(term);
        table.lengths.push_back(storedLength(textOf(data, term).size()));
    }
    const int bits = std::clamp(std::bit_width(pairs.size() / kEntriesPerBucket), 1, kMaxDirectoryBits);
    table.shift = 32 - bits;
    table.directory.assign((std::size_t{1} << static_cast<unsigned>(bits)) + 1, 0);
    for (const quint32 hash : table.hashes) {
        ++table.directory[(hash >> static_cast<unsigned>(table.shift)) + 1];
    }
    for (std::size_t b = 1; b < table.directory.size(); ++b) {
        table.directory[b] += table.directory[b - 1];
    }
    return table;
}

// The terms of `hash` whose length allows a match with a token of `length` units: every match of the token rules is
// within 2 units (a fuzzy or doubled-letter match within its budget of at most 2, a leet match at equal length).
template <typename Out>
void collect(const HashedTerms &table, quint32 hash, qsizetype length, Out &out)
{
    if (table.hashes.empty()) {
        return;
    }
    const int wanted = storedLength(length);
    const std::size_t bucket = hash >> static_cast<unsigned>(table.shift);
    const auto begin = table.hashes.begin() + table.directory[bucket];
    const auto end = table.hashes.begin() + table.directory[bucket + 1];
    for (auto it = std::lower_bound(begin, end, hash); it != end && *it == hash; ++it) {
        const auto i = static_cast<std::size_t>(it - table.hashes.begin());
        const int stored = table.lengths[i];
        if (std::abs(stored - wanted) <= 2 || (stored == kLongTerm && wanted >= kLongTerm - 2)) {
            out.push_back(table.terms[i]);
        }
    }
}

std::size_t hashedBytes(const HashedTerms &table)
{
    return ((table.hashes.capacity() + table.terms.capacity() + table.directory.capacity()) * sizeof(quint32)) +
           table.lengths.capacity();
}

} // namespace

TokenIndex::Builder::Builder(bool fuzzy)
    : m_data(std::make_unique<detail::TokenIndexData>())
{
    m_data->fuzzy = fuzzy;
}

TokenIndex::Builder::~Builder() = default;
TokenIndex::Builder::Builder(Builder &&) noexcept = default;
TokenIndex::Builder &TokenIndex::Builder::operator=(Builder &&) noexcept = default;

quint32 TokenIndex::Builder::add(QStringView term, quint32 document)
{
    if (term.isEmpty() || !m_data) {
        return kNoTerm;
    }
    TokenIndexData &data = *m_data;
    const auto count = static_cast<quint32>(data.termOffsets.size() - 1);
    if (2 * (static_cast<std::size_t>(count) + 1) > data.buckets.size()) {
        grow(data);
    }
    const std::size_t slot = probe(data, term, hashOf(term));
    quint32 id = data.buckets[slot];
    if (id == kNoTerm) {
        id = count;
        data.buckets[slot] = id;
        data.pool.insert(data.pool.end(), term.utf16(), term.utf16() + term.size());
        data.termOffsets.push_back(static_cast<quint32>(data.pool.size()));
    }
    data.pending.emplace_back(id, document);
    return id;
}

TokenIndex TokenIndex::Builder::build()
{
    const bool fuzzy = m_data ? m_data->fuzzy : true;
    auto data = std::exchange(m_data, std::make_unique<detail::TokenIndexData>());
    m_data->fuzzy = fuzzy;
    auto &pending = data->pending;
    std::ranges::sort(pending);
    const auto [first, last] = std::ranges::unique(pending);
    pending.erase(first, last);

    const std::size_t termCount = data->termOffsets.size() - 1;
    data->offsets.assign(termCount + 1, 0);
    for (const auto &[term, document] : pending) {
        ++data->offsets[term + 1];
    }
    for (std::size_t t = 0; t < termCount; ++t) {
        data->offsets[t + 1] += data->offsets[t];
    }
    data->documents.reserve(pending.size());
    for (const auto &[term, document] : pending) {
        data->documents.push_back(document);
    }
    pending.clear();
    pending.shrink_to_fit();
    data->pool.shrink_to_fit();
    data->termOffsets.shrink_to_fit();

    if (data->fuzzy) {
        std::vector<std::pair<quint32, quint32>> deletePairs;
        std::vector<std::pair<quint32, quint32>> leetPairs;
        for (std::size_t t = 0; t < termCount; ++t) {
            const auto term = static_cast<quint32>(t);
            const QStringView text = textOf(*data, term);
            if (const int budget = TokenRules::editBudget(text); budget > 0) {
                const DeleteHashes hashes = deleteHashes(text, budget);
                for (qsizetype k = 0; k < hashes.count; ++k) {
                    deletePairs.emplace_back(hashes.values[static_cast<std::size_t>(k)], term);
                }
            }
            bool changed = false;
            const quint64 folded = leetHash(text, changed);
            if (changed) {
                leetPairs.emplace_back(shortHash(folded), term);
            }
        }
        data->deletes = hashedTerms(std::move(deletePairs), *data);
        data->leet = hashedTerms(std::move(leetPairs), *data);
    }
    return TokenIndex(std::move(data));
}

TokenIndex::TokenIndex()
    : m_data(std::make_unique<detail::TokenIndexData>())
{
    m_data->offsets.push_back(0);
}

TokenIndex::TokenIndex(std::unique_ptr<detail::TokenIndexData> data)
    : m_data(std::move(data))
{}

TokenIndex::~TokenIndex() = default;
TokenIndex::TokenIndex(TokenIndex &&) noexcept = default;
TokenIndex &TokenIndex::operator=(TokenIndex &&) noexcept = default;

quint32 TokenIndex::find(QStringView term) const
{
    if (!m_data || term.isEmpty() || m_data->buckets.empty()) {
        return kNoTerm;
    }
    return m_data->buckets[probe(*m_data, term, hashOf(term))];
}

std::span<const quint32> TokenIndex::postings(quint32 term) const
{
    if (!m_data || term == kNoTerm || term >= m_data->termOffsets.size() - 1) {
        return {};
    }
    const quint32 begin = m_data->offsets[term];
    const quint32 end = m_data->offsets[static_cast<std::size_t>(term) + 1];
    return std::span<const quint32>(m_data->documents).subspan(begin, end - begin);
}

QStringView TokenIndex::term(quint32 term) const
{
    if (!m_data || term == kNoTerm || term >= m_data->termOffsets.size() - 1) {
        return {};
    }
    return textOf(*m_data, term);
}

qsizetype TokenIndex::termCount() const
{
    return m_data ? static_cast<qsizetype>(m_data->termOffsets.size() - 1) : 0;
}

QList<TokenIndex::FuzzyHit> TokenIndex::fuzzyLookup(QStringView token, qsizetype maxHits) const
{
    QList<FuzzyHit> hits;
    if (!m_data || token.isEmpty() || maxHits <= 0) {
        return hits;
    }
    QVarLengthArray<quint32, 64> candidates;
    // Leet forms: "deadmaus" finds "deadmau5" and the other way round.
    bool changed = false;
    collect(m_data->leet, shortHash(leetHash(token, changed)), token.size(), candidates);
    if (changed) {
        if (const quint32 plain = find(TokenRules::leetFold(token)); plain != kNoTerm) {
            candidates.push_back(plain);
        }
    }
    // Symmetric deletes of the first 7 characters within the token's budget (§17.6).
    if (const int budget = TokenRules::editBudget(token); budget > 0) {
        const DeleteHashes hashes = deleteHashes(token, budget);
        for (qsizetype k = 0; k < hashes.count; ++k) {
            collect(m_data->deletes, hashes.values[static_cast<std::size_t>(k)], token.size(), candidates);
        }
    }
    std::ranges::sort(candidates);
    const auto *const end = std::ranges::unique(candidates).begin();
    for (const auto *it = candidates.cbegin(); it != end; ++it) {
        const TokenComparison comparison = TokenRules::compare(token, term(*it));
        if (comparison.matched() && comparison.kind != TokenMatchKind::Exact) {
            hits.append({*it, comparison});
        }
    }
    std::ranges::sort(hits, [](FuzzyHit x, FuzzyHit y) {
        return std::pair(x.comparison.edits, x.term) < std::pair(y.comparison.edits, y.term);
    });
    if (hits.size() > maxHits) {
        hits.resize(maxHits);
    }
    return hits;
}

std::size_t TokenIndex::memoryUsage() const
{
    if (!m_data) {
        return 0;
    }
    const TokenIndexData &data = *m_data;
    std::size_t bytes = sizeof(TokenIndexData);
    bytes += data.pool.capacity() * sizeof(char16_t);
    bytes += (data.termOffsets.capacity() + data.buckets.capacity()) * sizeof(quint32);
    bytes += (data.offsets.capacity() + data.documents.capacity()) * sizeof(quint32);
    bytes += hashedBytes(data.deletes) + hashedBytes(data.leet);
    return bytes;
}

} // namespace selecta::match
