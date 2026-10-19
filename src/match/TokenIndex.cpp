// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/TokenIndex.h"

#include <QString>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace selecta::match {

namespace detail {

struct TokenIndexData
{
    std::unordered_map<std::u16string, quint32> ids;  // term → id; node-based, so keys never move
    std::vector<std::u16string_view> terms;           // id → text (views into `ids`)
    std::vector<std::pair<quint32, quint32>> pending; // (term, document) while building

    std::vector<quint32> offsets; // postings of term t: documents[offsets[t], offsets[t + 1])
    std::vector<quint32> documents;
    std::vector<quint64> deleteHashes; // sorted; deleteTerms[i] is the term of deleteHashes[i]
    std::vector<quint32> deleteTerms;
    std::vector<quint64> leetHashes; // leet-folded forms of terms that mix letters and digits
    std::vector<quint32> leetTerms;
};

} // namespace detail

namespace {

std::u16string_view toView(QStringView text)
{
    return {text.utf16(), static_cast<std::size_t>(text.size())};
}

QStringView toQt(std::u16string_view text)
{
    return {text.data(), static_cast<qsizetype>(text.size())};
}

// FNV-1a over UTF-16 code units. Collisions only add candidates: every candidate is verified.
quint64 hashOf(std::u16string_view text)
{
    quint64 hash = 0xCBF29CE484222325ULL;
    for (const char16_t unit : text) {
        hash ^= static_cast<quint64>(unit);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

// Every distinct string made by deleting up to `budget` code units from `text` (text itself included).
std::vector<std::u16string> deletes(std::u16string_view text, int budget)
{
    std::vector<std::u16string> all{std::u16string(text)};
    std::size_t levelBegin = 0;
    for (int level = 0; level < budget; ++level) {
        const std::size_t levelEnd = all.size();
        for (std::size_t k = levelBegin; k < levelEnd; ++k) {
            const std::u16string source = all[k];
            for (std::size_t i = 0; i < source.size(); ++i) {
                std::u16string shorter = source;
                shorter.erase(i, 1);
                if (std::ranges::find(all, shorter) == all.end()) {
                    all.push_back(std::move(shorter));
                }
            }
        }
        levelBegin = levelEnd;
    }
    return all;
}

std::u16string_view deletePrefix(std::u16string_view term)
{
    return term.substr(0, std::min<std::size_t>(term.size(), TokenIndex::kDeletePrefix));
}

void sortPairs(std::vector<std::pair<quint64, quint32>> &pairs, std::vector<quint64> &hashes,
               std::vector<quint32> &terms)
{
    std::ranges::sort(pairs);
    const auto [first, last] = std::ranges::unique(pairs);
    pairs.erase(first, last);
    hashes.reserve(pairs.size());
    terms.reserve(pairs.size());
    for (const auto &[hash, term] : pairs) {
        hashes.push_back(hash);
        terms.push_back(term);
    }
}

void collect(const std::vector<quint64> &hashes, const std::vector<quint32> &terms, quint64 hash,
             std::vector<quint32> &out)
{
    const auto range = std::ranges::equal_range(hashes, hash);
    for (auto it = range.begin(); it != range.end(); ++it) {
        out.push_back(terms[static_cast<std::size_t>(it - hashes.begin())]);
    }
}

} // namespace

TokenIndex::Builder::Builder()
    : m_data(std::make_unique<detail::TokenIndexData>())
{}

TokenIndex::Builder::~Builder() = default;
TokenIndex::Builder::Builder(Builder &&) noexcept = default;
TokenIndex::Builder &TokenIndex::Builder::operator=(Builder &&) noexcept = default;

void TokenIndex::Builder::add(QStringView term, quint32 document)
{
    if (term.isEmpty() || !m_data) {
        return;
    }
    const auto [it, inserted] =
        m_data->ids.try_emplace(std::u16string(toView(term)), static_cast<quint32>(m_data->terms.size()));
    if (inserted) {
        m_data->terms.push_back(it->first);
    }
    m_data->pending.emplace_back(it->second, document);
}

TokenIndex TokenIndex::Builder::build()
{
    auto data = std::exchange(m_data, std::make_unique<detail::TokenIndexData>());
    auto &pending = data->pending;
    std::ranges::sort(pending);
    const auto [first, last] = std::ranges::unique(pending);
    pending.erase(first, last);

    const std::size_t termCount = data->terms.size();
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

    std::vector<std::pair<quint64, quint32>> deletePairs;
    std::vector<std::pair<quint64, quint32>> leetPairs;
    for (std::size_t t = 0; t < termCount; ++t) {
        const std::u16string_view term = data->terms[t];
        const QStringView qtTerm = toQt(term);
        if (const int budget = TokenRules::editBudget(qtTerm); budget > 0) {
            for (const std::u16string &variant : deletes(deletePrefix(term), budget)) {
                deletePairs.emplace_back(hashOf(variant), static_cast<quint32>(t));
            }
        }
        const QString folded = TokenRules::leetFold(qtTerm);
        if (folded != qtTerm) {
            leetPairs.emplace_back(hashOf(toView(folded)), static_cast<quint32>(t));
        }
    }
    sortPairs(deletePairs, data->deleteHashes, data->deleteTerms);
    sortPairs(leetPairs, data->leetHashes, data->leetTerms);
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
    if (!m_data || term.isEmpty()) {
        return kNoTerm;
    }
    // std::unordered_map has no heterogeneous lookup for std::u16string here; the copy is short-lived.
    const auto it = m_data->ids.find(std::u16string(toView(term)));
    return it == m_data->ids.end() ? kNoTerm : it->second;
}

std::span<const quint32> TokenIndex::postings(quint32 term) const
{
    if (!m_data || term == kNoTerm || term >= m_data->terms.size()) {
        return {};
    }
    const quint32 begin = m_data->offsets[term];
    const quint32 end = m_data->offsets[static_cast<std::size_t>(term) + 1];
    return std::span<const quint32>(m_data->documents).subspan(begin, end - begin);
}

QStringView TokenIndex::term(quint32 term) const
{
    if (!m_data || term == kNoTerm || term >= m_data->terms.size()) {
        return {};
    }
    return toQt(m_data->terms[term]);
}

qsizetype TokenIndex::termCount() const
{
    return m_data ? static_cast<qsizetype>(m_data->terms.size()) : 0;
}

QList<TokenIndex::FuzzyHit> TokenIndex::fuzzyLookup(QStringView token, qsizetype maxHits) const
{
    QList<FuzzyHit> hits;
    if (!m_data || token.isEmpty() || maxHits <= 0) {
        return hits;
    }
    std::vector<quint32> candidates;
    // Leet forms: "deadmaus" finds "deadmau5" and the other way round.
    const QString folded = TokenRules::leetFold(token);
    collect(m_data->leetHashes, m_data->leetTerms, hashOf(toView(folded)), candidates);
    if (folded != token) {
        if (const quint32 plain = find(folded); plain != kNoTerm) {
            candidates.push_back(plain);
        }
    }
    // Symmetric deletes of the first 7 characters within the token's budget (§17.6).
    if (const int budget = TokenRules::editBudget(token); budget > 0) {
        for (const std::u16string &variant : deletes(deletePrefix(toView(token)), budget)) {
            collect(m_data->deleteHashes, m_data->deleteTerms, hashOf(variant), candidates);
        }
    }
    std::ranges::sort(candidates);
    const auto [first, last] = std::ranges::unique(candidates);
    candidates.erase(first, last);
    for (const quint32 candidate : candidates) {
        const TokenComparison comparison = TokenRules::compare(token, term(candidate));
        if (comparison.matched() && comparison.kind != TokenMatchKind::Exact) {
            hits.append({candidate, comparison});
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
    std::size_t bytes = sizeof(detail::TokenIndexData);
    for (const auto &[text, id] : m_data->ids) {
        // node, key object and its heap buffer beyond the small-string capacity, and the bucket
        bytes += 48 + (text.size() > 10 ? (text.size() + 1) * sizeof(char16_t) : 0);
    }
    bytes += m_data->terms.capacity() * sizeof(std::u16string_view);
    bytes += m_data->offsets.capacity() * sizeof(quint32);
    bytes += m_data->documents.capacity() * sizeof(quint32);
    bytes += (m_data->deleteHashes.capacity() * sizeof(quint64)) + (m_data->deleteTerms.capacity() * sizeof(quint32));
    bytes += (m_data->leetHashes.capacity() * sizeof(quint64)) + (m_data->leetTerms.capacity() * sizeof(quint32));
    return bytes;
}

} // namespace selecta::match
