// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Similarity.h"

#include <QList>
#include <QStringView>

#include <cstddef>
#include <memory>
#include <span>

namespace selecta::match {

namespace detail {
struct TokenIndexData;
}

// Exact postings plus a symmetric-delete fuzzy index over a vocabulary of terms (note §10, §17.6).
//
// Terms are normalised tokens or squashed keys; documents are dense ids chosen by the owner (MatchIndex record
// indexes). The fuzzy side indexes, for every term with a non-zero edit budget, the deletes of its first 7 characters
// up to its budget (TokenRules); a lookup generates the deletes of the query the same way and verifies every
// candidate with TokenRules::compare, so it returns exactly the terms the token rules accept, transpositions
// included.
//
// Threading: immutable once built. All const members may be called concurrently from any thread; share it as
// std::shared_ptr<const TokenIndex> or inside a const owner. The Builder is single-threaded.
// Failure modes: none. An index built from nothing is empty and finds nothing. Terms longer than
// kMaxFuzzyTokenLength are indexed for exact lookups only.
class TokenIndex final
{
public:
    static constexpr quint32 kNoTerm = 0xFFFFFFFFU;
    static constexpr qsizetype kDeletePrefix = 7; // SymSpell prefix length (note §17.6)

    class Builder
    {
    public:
        // `fuzzy` false builds no delete index: exact lookups only (squashed keys, history keys).
        explicit Builder(bool fuzzy = true);
        ~Builder();
        Builder(const Builder &) = delete;
        Builder &operator=(const Builder &) = delete;
        Builder(Builder &&) noexcept;
        Builder &operator=(Builder &&) noexcept;

        // Adds `term` to the postings of `document` and returns the term's id (kNoTerm for an empty term, which is
        // ignored). Adding the same pair twice is harmless. Ids are dense and stay valid in the built index.
        quint32 add(QStringView term, quint32 document);

        // Sorts and deduplicates the postings and builds the delete index. The builder is empty afterwards.
        TokenIndex build();

    private:
        std::unique_ptr<detail::TokenIndexData> m_data;
    };

    struct FuzzyHit
    {
        quint32 term = kNoTerm;
        TokenComparison comparison;
    };

    TokenIndex(); // empty
    ~TokenIndex();
    TokenIndex(const TokenIndex &) = delete;
    TokenIndex &operator=(const TokenIndex &) = delete;
    TokenIndex(TokenIndex &&) noexcept;
    TokenIndex &operator=(TokenIndex &&) noexcept;

    // The id of an exact term, or kNoTerm.
    quint32 find(QStringView term) const;

    // The sorted, unique documents of a term; empty for kNoTerm or an unknown id. Valid as long as the index lives.
    std::span<const quint32> postings(quint32 term) const;

    // The text of a term; empty for an unknown id. Valid as long as the index lives.
    QStringView term(quint32 term) const;

    qsizetype termCount() const;

    // Vocabulary terms the token rules accept for `token`, other than the token itself (§17.3), fewest edits first,
    // then by term id; at most `maxHits`. A token with a zero budget (≤ 3 characters, digits) only finds leet forms.
    QList<FuzzyHit> fuzzyLookup(QStringView token, qsizetype maxHits = 16) const;

    // Approximate heap use in bytes.
    std::size_t memoryUsage() const;

private:
    explicit TokenIndex(std::unique_ptr<detail::TokenIndexData> data);

    std::unique_ptr<detail::TokenIndexData> m_data;
};

} // namespace selecta::match
