// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Similarity.h"

#include <QList>
#include <QStringView>
#include <QVarLengthArray>

#include <span>

// Internal: token alignment shared by FieldMatcher (two fields), the reference scorer and the matcher's token-bag
// route (a reference field against the zones of a path). Pure; any thread.
namespace selecta::match::detail {

// What alignment needs to know about a token besides its text, computed once per token (MatchIndex keeps it per
// vocabulary term), so the hot loops scan nothing.
struct TokenFacts
{
    quint16 length = 0;  // code points (TokenRules::length), at most 65535
    bool light = false;  // a light word ("the", "and", "feat"): weighs 1
    bool digits = false; // has a digit: matches exactly or through leet folding only (§17.3)
};

TokenFacts tokenFacts(QStringView token);

// One token to align. Joined pairs (§17.4 step 3) are only formed inside one segment; ignorable tokens (track numbers,
// junk, version words) still align but don't count as unexplained.
struct AlignToken
{
    QStringView text;
    qint16 segment = 0;
    bool ignorable = false;
    TokenFacts facts; // of `text`

    // Weight in coverage sums: the length in code points, or 1 for light words.
    double weight() const { return facts.light ? 1.0 : static_cast<double>(facts.length); }
};

// A token with its facts computed.
AlignToken alignToken(QStringView text, int segment = 0, bool ignorable = false);

// The alignment of A against B: per-token similarity on both sides (0 when unmatched), and the accepted pairs.
struct Alignment
{
    QVarLengthArray<double, kMaxFieldTokens> scoreA; // per token of A, in [0, 1]
    QVarLengthArray<double, kMaxFieldTokens> scoreB; // per token of B
    QVarLengthArray<TokenPair, kMaxFieldTokens> pairs;
    int edits = 0;
    bool fuzzy = false;
    double weightA = 0.0; // Σ weights of A
    double weightB = 0.0;
    double matchedA = 0.0; // Σ weight × score over A
    double matchedB = 0.0;

    double coverageA() const { return weightA > 0.0 ? matchedA / weightA : 0.0; }
    double coverageB() const { return weightB > 0.0 ? matchedB / weightB : 0.0; }
    double similarity() const { return weightA + weightB > 0.0 ? (matchedA + matchedB) / (weightA + weightB) : 0.0; }
};

// Aligns A and B under the §17.3 rules and the field conditions (corroboration, one fuzzy token in a field of one or
// two tokens, the field budget over min(squashed A, squashed B)). Greedy in both directions; the better one wins, so
// the result's similarity is symmetric. At most kMaxFieldTokens tokens of each side are used.
Alignment align(std::span<const AlignToken> a, std::span<const AlignToken> b, FieldMatchOptions options);

// Tokens kept inline for the common case: fields rarely have more than 16 words.
using AlignTokens = QVarLengthArray<AlignToken, 16>;

// Appends the tokens of a normalised key as AlignTokens of one segment (views into `key`).
void appendAlignTokens(AlignTokens &out, QStringView key, int segment = 0, bool ignorable = false);

// A field as views into storage that outlives the comparison: its tokens and its squashed and leet keys. MatchIndex
// records give views into the index's vocabulary, so scoring a candidate allocates nothing.
struct FieldView
{
    AlignTokens tokens;
    QStringView squashed;
    QStringView leet; // empty: the same as squashed

    bool isEmpty() const { return tokens.isEmpty(); }
};

// Views into a FieldKey (which must outlive the view).
FieldView fieldView(const FieldKey &key);

// FieldMatcher's comparison on views (see FieldMatcher::compare). Without `withPairs` the result's pairs stay empty
// (no allocation on the matcher's hot path).
FieldMatch compareFields(const FieldView &a, const FieldView &b, FieldMatchOptions options, bool withPairs = true);

// Upper bounds for pruning, computed without any edit distance: each token is credited with the best score it could
// reach (1 for an equal, leet-equal or joined match, 1 − 0.15 when lengths and first letters leave room for a typo),
// as if no other token competed for it and no field condition applied.
//
// coverageBound(a, b) ≥ align(a, b, any options).coverageA(); similarityBound(a, b) ≥ compareFields(a, b, any
// options).similarity, the squashed and leet routes included. Pure; any thread; allocates nothing for tokens without
// digits.
double coverageBound(std::span<const AlignToken> a, std::span<const AlignToken> b);
double similarityBound(const FieldView &a, const FieldView &b);

} // namespace selecta::match::detail
