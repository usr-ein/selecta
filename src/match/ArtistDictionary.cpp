// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/ArtistDictionary.h"

#include "match/Normalizer.h"
#include "match/Similarity.h"
#include "match/TokenIndex.h"

#include <QHash>

#include <algorithm>
#include <limits>
#include <tuple>

namespace selecta::match {

namespace detail {

struct ArtistDictionaryData
{
    QHash<QString, int> wholeWeights;  // squashed artist key → weight
    QHash<QString, int> memberWeights; // squashed key of one member of a multi-artist name → weight
    TokenIndex fuzzy;                  // every key above as a term (documents unused)
    qsizetype size = 0;
};

} // namespace detail

namespace {

constexpr qsizetype kMinMemberLength = 3; // "a", "x" or "dj" alone are no evidence of anything

void addWeight(QHash<QString, int> &weights, const QString &key, int weight)
{
    int &total = weights[key];
    total = std::min(std::numeric_limits<int>::max() - weight, total) + weight; // saturating
}

// Better: exact before fuzzy (fewer edits), then whole-name, then weight.
bool better(ArtistDictionary::Hit candidate, ArtistDictionary::Hit best)
{
    if (!candidate.found()) {
        return false;
    }
    if (!best.found()) {
        return true;
    }
    return std::make_tuple(-candidate.edits, !candidate.memberOnly, candidate.weight) >
           std::make_tuple(-best.edits, !best.memberOnly, best.weight);
}

} // namespace

ArtistDictionary::ArtistDictionary()
    : m_data(std::make_unique<detail::ArtistDictionaryData>())
{}

ArtistDictionary::ArtistDictionary(const QList<Entry> &entries)
    : m_data(std::make_unique<detail::ArtistDictionaryData>())
{
    TokenIndex::Builder builder;
    for (const Entry &entry : entries) {
        const ArtistKeys keys = Normalizer::artistKeys(entry.name);
        if (keys.field.squashed.isEmpty()) {
            continue;
        }
        const int weight = std::max(1, entry.weight);
        addWeight(m_data->wholeWeights, keys.field.squashed, weight);
        builder.add(keys.field.squashed, 0);
        if (keys.set.size() > 1) {
            for (const QString &member : keys.set) {
                const QString squashed = Normalizer::squash(member);
                if (TokenRules::length(squashed) >= kMinMemberLength) {
                    addWeight(m_data->memberWeights, squashed, weight);
                    builder.add(squashed, 0);
                }
            }
        }
    }
    m_data->size = m_data->wholeWeights.size();
    m_data->fuzzy = builder.build();
}

ArtistDictionary::~ArtistDictionary() = default;
ArtistDictionary::ArtistDictionary(ArtistDictionary &&) noexcept = default;
ArtistDictionary &ArtistDictionary::operator=(ArtistDictionary &&) noexcept = default;

ArtistDictionary::Hit ArtistDictionary::lookup(QStringView artist) const
{
    Hit best;
    if (!m_data || m_data->size == 0) {
        return best;
    }
    const ArtistKeys keys = Normalizer::artistKeys(artist);
    if (keys.field.squashed.isEmpty()) {
        return best;
    }
    QStringList queries{keys.field.squashed};
    if (keys.set.size() > 1) {
        for (const QString &member : keys.set) {
            const QString squashed = Normalizer::squash(member);
            if (TokenRules::length(squashed) >= kMinMemberLength) {
                queries.append(squashed);
            }
        }
    }
    const auto weightOf = [this](const QString &key, bool &memberOnly) {
        if (const auto it = m_data->wholeWeights.constFind(key); it != m_data->wholeWeights.cend()) {
            memberOnly = false;
            return it.value();
        }
        memberOnly = true;
        return m_data->memberWeights.value(key, 0);
    };
    for (const QString &query : std::as_const(queries)) {
        bool memberOnly = false;
        const int weight = weightOf(query, memberOnly);
        const Hit hit{weight, 0, memberOnly || query != keys.field.squashed};
        if (better(hit, best)) {
            best = hit;
        }
    }
    if (best.exact()) {
        return best;
    }
    for (const QString &query : std::as_const(queries)) {
        const QList<TokenIndex::FuzzyHit> hits = m_data->fuzzy.fuzzyLookup(query);
        for (const TokenIndex::FuzzyHit &fuzzy : hits) {
            const TokenComparison &comparison = fuzzy.comparison;
            const QString term = m_data->fuzzy.term(fuzzy.term).toString();
            const qsizetype shorter = std::min(TokenRules::length(query), TokenRules::length(term));
            if (comparison.needsCorroboration || comparison.edits > TokenRules::fieldEditBudget(shorter)) {
                continue;
            }
            bool memberOnly = false;
            const int weight = weightOf(term, memberOnly);
            const Hit hit{weight, comparison.edits, memberOnly || query != keys.field.squashed};
            if (better(hit, best)) {
                best = hit;
            }
        }
    }
    return best;
}

qsizetype ArtistDictionary::size() const
{
    return m_data ? m_data->size : 0;
}

std::size_t ArtistDictionary::memoryUsage() const
{
    if (!m_data) {
        return 0;
    }
    constexpr std::size_t kPerHashEntry = 64; // node, key and a short key's heap buffer, roughly
    return sizeof(detail::ArtistDictionaryData) +
           (static_cast<std::size_t>(m_data->wholeWeights.size() + m_data->memberWeights.size()) * kPerHashEntry) +
           m_data->fuzzy.memoryUsage();
}

} // namespace selecta::match
