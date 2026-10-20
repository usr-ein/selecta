// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Similarity.h"

#include <QList>
#include <QStringView>

#include <span>

// Internal: token alignment shared by FieldMatcher (two fields), the reference scorer and the matcher's token-bag
// route (a reference field against the zones of a path). Pure; any thread.
namespace selecta::match::detail {

// One token to align. Joined pairs (§17.4 step 3) are only formed inside one segment; ignorable tokens (track numbers,
// junk, version words) still align but don't count as unexplained.
struct AlignToken
{
    QStringView text;
    int segment = 0;
    bool ignorable = false;
};

// The alignment of A against B: per-token similarity on both sides (0 when unmatched), and the accepted pairs.
struct Alignment
{
    QList<double> scoreA; // per token of A, in [0, 1]
    QList<double> scoreB; // per token of B
    QList<TokenPair> pairs;
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

// Token weight for coverage sums: the length in code points, or 1 for light words ("the", "and", "feat").
double tokenWeight(QStringView token);

// Aligns A and B under the §17.3 rules and the field conditions (corroboration, one fuzzy token in a field of one or
// two tokens, the field budget over min(squashed A, squashed B)). Greedy in both directions; the better one wins, so
// the result's similarity is symmetric. At most kMaxFieldTokens tokens of each side are used.
Alignment align(std::span<const AlignToken> a, std::span<const AlignToken> b, FieldMatchOptions options);

// Tokens of a normalised key as AlignTokens of one segment (views into `key`).
QList<AlignToken> alignTokens(QStringView key, int segment = 0);

// The digits of a string in order ("blink182" → "182"): the squashed route requires equal digits.
QString digitsOf(QStringView text);

} // namespace selecta::match::detail
