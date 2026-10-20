// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Similarity.h"

#include "match/Lexicon.h"
#include "match/detail/Alignment.h"

#include <QVarLengthArray>

#include <algorithm>
#include <cstdlib>
#include <rapidfuzz/distance/OSA.hpp>
#include <rapidfuzz/fuzz.hpp>

namespace selecta::match {

namespace {

bool hasDigit(QStringView token)
{
    return std::ranges::any_of(token, [](QChar c) { return c.isDigit(); });
}

bool hasLetter(QStringView token)
{
    return std::ranges::any_of(token, [](QChar c) { return c.isLetter(); });
}

int budgetForLength(qsizetype length)
{
    if (length <= 3) {
        return 0;
    }
    if (length <= 8) {
        return 1;
    }
    return 2;
}

struct Osa
{
    int edits = 0;
    int substitutions = 0;
};

// Optimal string alignment (restricted Damerau-Levenshtein) over UTF-16 code units, choosing among the cheapest edit
// scripts the one with the fewest substitutions. Inputs are short (≤ 2 × kMaxFuzzyTokenLength units).
Osa osa(QStringView a, QStringView b)
{
    const qsizetype n = a.size();
    const qsizetype m = b.size();
    using Cost = std::pair<int, int>; // (edits, substitutions), compared lexicographically
    QVarLengthArray<Cost, 1024> d((n + 1) * (m + 1));
    const auto at = [m](qsizetype i, qsizetype j) { return (i * (m + 1)) + j; };
    for (qsizetype i = 0; i <= n; ++i) {
        d[at(i, 0)] = {static_cast<int>(i), 0};
    }
    for (qsizetype j = 0; j <= m; ++j) {
        d[at(0, j)] = {static_cast<int>(j), 0};
    }
    for (qsizetype i = 1; i <= n; ++i) {
        for (qsizetype j = 1; j <= m; ++j) {
            const bool same = a[i - 1] == b[j - 1];
            Cost best = d[at(i - 1, j - 1)];
            if (!same) {
                best = {best.first + 1, best.second + 1};
            }
            const Cost deletion{d[at(i - 1, j)].first + 1, d[at(i - 1, j)].second};
            const Cost insertion{d[at(i, j - 1)].first + 1, d[at(i, j - 1)].second};
            best = std::min({best, deletion, insertion});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                const Cost transposition{d[at(i - 2, j - 2)].first + 1, d[at(i - 2, j - 2)].second};
                best = std::min(best, transposition);
            }
            d[at(i, j)] = best;
        }
    }
    return {d[at(n, m)].first, d[at(n, m)].second};
}

// The first two letters swapped and nothing else ("bciep"/"biecp" aside): allowed despite the first-letter rule.
bool isLeadingTransposition(QStringView a, QStringView b)
{
    return a.size() == b.size() && a.size() >= 2 && a[0] == b[1] && a[1] == b[0] && a.sliced(2) == b.sliced(2);
}

} // namespace

qsizetype TokenRules::length(QStringView token)
{
    qsizetype count = 0;
    for (qsizetype i = 0; i < token.size(); ++i) {
        const bool secondHalf = token[i].isLowSurrogate() && i > 0 && token[i - 1].isHighSurrogate();
        if (!secondHalf) {
            ++count;
        }
    }
    return count;
}

int TokenRules::editBudget(QStringView token)
{
    const qsizetype len = length(token);
    if (len > kMaxFuzzyTokenLength || hasDigit(token)) {
        return 0;
    }
    return budgetForLength(len);
}

int TokenRules::fieldEditBudget(qsizetype squashedLength)
{
    return squashedLength <= 12 ? 1 : 2;
}

QString TokenRules::skeleton(QStringView token)
{
    QString out;
    out.reserve(token.size());
    for (const QChar c : token) {
        if (out.isEmpty() || out.back() != c) {
            out.append(c);
        }
    }
    return out;
}

QString TokenRules::leetFold(QStringView token)
{
    if (!hasLetter(token) || !hasDigit(token)) {
        return token.toString();
    }
    QString folded = token.toString();
    for (QChar &c : folded) {
        if (const char16_t replacement = detail::lexicon::leetFold(c.unicode()); replacement != 0 && c.isDigit()) {
            c = QChar(replacement);
        }
    }
    return folded;
}

TokenComparison TokenRules::compare(QStringView a, QStringView b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return {};
    }
    if (a == b) {
        return {TokenMatchKind::Exact, 0, false, false};
    }
    // Digits are never edited (§17.3): tokens with digits match exactly or through leet folding only.
    if (hasDigit(a) || hasDigit(b)) {
        if ((hasLetter(a) || hasLetter(b)) && leetFold(a) == leetFold(b)) {
            return {TokenMatchKind::Leet, 0, false, false};
        }
        return {};
    }
    const qsizetype lengthA = length(a);
    const qsizetype lengthB = length(b);
    const qsizetype shorter = std::min(lengthA, lengthB);
    const int budget = budgetForLength(shorter);
    if (budget == 0 || shorter > kMaxFuzzyTokenLength || std::abs(lengthA - lengthB) > budget) {
        return {};
    }
    // Doubled letters (≥ 5 characters): "gallaxy" = "galaxy", one edit, no corroboration needed.
    if (shorter >= 5 && skeleton(a) == skeleton(b)) {
        return {TokenMatchKind::Skeleton, 1, false, false};
    }
    // Bit-parallel pre-filter: most pairs fail here.
    const auto cutoff = static_cast<std::size_t>(budget);
    const std::size_t prefilter =
        rapidfuzz::osa_distance(a.utf16(), a.utf16() + a.size(), b.utf16(), b.utf16() + b.size(), cutoff);
    if (prefilter > cutoff) {
        return {};
    }
    const Osa script = osa(a, b);
    if (script.edits == 0 || script.edits > budget) {
        return {};
    }
    if (shorter <= 8 && a.front() != b.front() && !isLeadingTransposition(a, b)) {
        return {}; // same first letter up to 8 characters (Pollock & Zamora: 7.8 % of typos hit it)
    }
    TokenComparison comparison{TokenMatchKind::Fuzzy, script.edits, script.substitutions > 0, false};
    if (shorter <= 5) {
        if (comparison.substitution) {
            return {}; // 4–5 characters: no substitution ("noon"/"neon", "live"/"love")
        }
        comparison.needsCorroboration = true;
    } else if (shorter <= 8) {
        comparison.needsCorroboration = comparison.substitution;
    } else if (script.substitutions > 1) {
        return {};
    }
    return comparison;
}

FieldMatch FieldMatcher::compare(const FieldKey &a, const FieldKey &b, FieldMatchOptions options)
{
    return detail::compareFields(detail::fieldView(a), detail::fieldView(b), options);
}

FieldMatch FieldMatcher::compareKeys(QStringView a, QStringView b, FieldMatchOptions options)
{
    FieldKey keyA{a.toString(), Normalizer::squash(a), {}};
    FieldKey keyB{b.toString(), Normalizer::squash(b), {}};
    keyA.leet = keyA.squashed;
    keyB.leet = keyB.squashed;
    return compare(keyA, keyB, options);
}

namespace detail {

namespace {

bool sameTokens(const FieldView &a, const FieldView &b)
{
    if (a.tokens.size() != b.tokens.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.tokens.size(); ++i) {
        if (a.tokens.at(i).text != b.tokens.at(i).text) {
            return false;
        }
    }
    return true;
}

FieldMatch exactMatch(FieldMatch::Route route)
{
    FieldMatch match;
    match.similarity = 1.0;
    match.coverageA = 1.0;
    match.coverageB = 1.0;
    match.exact = true;
    match.route = route;
    return match;
}

// The squashed keys as one token, for concatenations the pair join misses ("okumadrft" against "okuma drift"):
// equal digits, fuzz::ratio ≥ 85, and the token rules under the field budget.
void trySquashedRoute(FieldMatch &match, QStringView a, QStringView b, FieldMatchOptions options)
{
    if (match.exact || a.isEmpty() || b.isEmpty() || digitsOf(a) != digitsOf(b)) {
        return;
    }
    const double ratio =
        rapidfuzz::fuzz::ratio(a.utf16(), a.utf16() + a.size(), b.utf16(), b.utf16() + b.size(), kSquashedRatioMin);
    if (ratio < kSquashedRatioMin) {
        return;
    }
    const TokenComparison whole = TokenRules::compare(a, b);
    const qsizetype shorter = std::min(TokenRules::length(a), TokenRules::length(b));
    const bool allowed = whole.matched() && whole.edits <= TokenRules::fieldEditBudget(shorter) &&
                         (!whole.needsCorroboration || options.durationCorroborates);
    if (allowed && whole.score() > match.similarity) {
        match.similarity = whole.score();
        match.coverageA = whole.score();
        match.coverageB = whole.score();
        match.unexplainedB = 0.0;
        match.edits = whole.edits;
        match.fuzzy = true;
        match.exact = false;
        match.route = FieldMatch::Route::SquashedFuzzy;
        match.pairs.clear();
    }
}

} // namespace

FieldMatch compareFields(const FieldView &a, const FieldView &b, FieldMatchOptions options)
{
    if (a.isEmpty() || b.isEmpty()) {
        return {};
    }
    if (!a.squashed.isEmpty() && a.squashed == b.squashed) {
        return exactMatch(sameTokens(a, b) ? FieldMatch::Route::Tokens : FieldMatch::Route::Squashed);
    }
    const QStringView leetA = a.leet.isEmpty() ? a.squashed : a.leet;
    const QStringView leetB = b.leet.isEmpty() ? b.squashed : b.leet;
    if (!leetA.isEmpty() && leetA == leetB) {
        return exactMatch(FieldMatch::Route::Leet);
    }

    const Alignment alignment = align(std::span(a.tokens.data(), static_cast<std::size_t>(a.tokens.size())),
                                      std::span(b.tokens.data(), static_cast<std::size_t>(b.tokens.size())), options);
    FieldMatch match;
    match.similarity = alignment.similarity();
    match.coverageA = alignment.coverageA();
    match.coverageB = alignment.coverageB();
    match.edits = alignment.edits;
    match.fuzzy = alignment.fuzzy;
    match.pairs = alignment.pairs;
    match.route = alignment.pairs.isEmpty() ? FieldMatch::Route::None : FieldMatch::Route::Tokens;
    match.exact = match.similarity >= 1.0 && match.edits == 0;
    qsizetype unexplained = 0;
    qsizetype total = 0;
    for (qsizetype j = 0; j < b.tokens.size(); ++j) {
        const qsizetype len = TokenRules::length(b.tokens.at(j).text);
        total += len;
        if (j >= alignment.scoreB.size() || alignment.scoreB.at(j) <= 0.0) {
            unexplained += len;
        }
    }
    match.unexplainedB = total > 0 ? static_cast<double>(unexplained) / static_cast<double>(total) : 0.0;
    trySquashedRoute(match, a.squashed, b.squashed, options);
    return match;
}

} // namespace detail

} // namespace selecta::match
