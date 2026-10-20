// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/Alignment.h"

#include "match/Lexicon.h"

#include <QString>

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace selecta::match::detail {

namespace {

struct Candidate
{
    TokenPair pair;
    int covered = 0;   // characters covered on both sides
    QStringView textA; // for a symmetric tie-break (the first token of a joined pair)
    QStringView textB;
};

bool isExactKind(TokenMatchKind kind)
{
    return kind == TokenMatchKind::Exact || kind == TokenMatchKind::Leet || kind == TokenMatchKind::Joined;
}

// `whole` equals `first` followed by `second`, without building the joined string.
bool equalsJoined(QStringView whole, QStringView first, QStringView second)
{
    return whole.size() == first.size() + second.size() && whole.startsWith(first) &&
           whole.sliced(first.size()) == second;
}

// Necessary conditions of a fuzzy TokenRules::compare() match between two different tokens without digits, from
// the facts and the first letters only: the shorter token's budget, the length gap, and the first-letter rule (a
// doubled-letter match keeps the first letter too).
bool fuzzyPossible(const AlignToken &a, const AlignToken &b)
{
    const int shorter = std::min(a.facts.length, b.facts.length);
    int budget = 2; // the §17.3 budget of the shorter token
    if (shorter <= 3) {
        budget = 0;
    } else if (shorter <= 8) {
        budget = 1;
    }
    if (budget == 0 || shorter > kMaxFuzzyTokenLength || std::abs(a.facts.length - b.facts.length) > budget) {
        return false;
    }
    const bool leadingSwap = a.text[0] == b.text[1] && a.text[1] == b.text[0];
    return shorter > 8 || a.text.front() == b.text.front() || leadingSwap;
}

// Whether TokenRules::compare(a, b) could match: false means it certainly doesn't.
bool mayMatch(const AlignToken &a, const AlignToken &b)
{
    if (a.facts.digits || b.facts.digits) {
        return a.text.size() == b.text.size(); // exact or leet only, and leet folding keeps the length
    }
    return a.text == b.text || fuzzyPossible(a, b);
}

// The best score two tokens could reach under the §17.3 rules, without computing an edit distance.
double tokenBound(const AlignToken &a, const AlignToken &b)
{
    if (a.text == b.text) {
        return 1.0;
    }
    if (a.facts.digits || b.facts.digits) {
        return a.text.size() == b.text.size() ? TokenRules::compare(a.text, b.text).score() : 0.0;
    }
    return fuzzyPossible(a, b) ? 1.0 - kFuzzyEditPenalty : 0.0;
}

// The best score token i of `own` could reach against `other`, alone or as part of a joined pair.
double bestPossible(std::span<const AlignToken> own, qsizetype i, std::span<const AlignToken> other)
{
    const auto n = static_cast<qsizetype>(own.size());
    const auto m = static_cast<qsizetype>(other.size());
    const QStringView token = own[i].text;
    double best = 0.0;
    for (qsizetype j = 0; j < m && best < 1.0; ++j) {
        best = std::max(best, tokenBound(own[i], other[j]));
        if (j + 1 < m && equalsJoined(token, other[j].text, other[j + 1].text)) {
            best = 1.0;
        }
    }
    for (qsizetype k = std::max<qsizetype>(i - 1, 0); best < 1.0 && k <= i && k + 1 < n; ++k) {
        for (qsizetype j = 0; j < m; ++j) {
            if (equalsJoined(other[j].text, own[k].text, own[k + 1].text)) {
                best = 1.0;
                break;
            }
        }
    }
    return best;
}

// The tokens align() looks at.
std::span<const AlignToken> aligned(std::span<const AlignToken> tokens)
{
    return tokens.first(std::min<std::size_t>(tokens.size(), kMaxFieldTokens));
}

double totalWeight(std::span<const AlignToken> tokens)
{
    double weight = 0.0;
    for (const AlignToken &token : aligned(tokens)) {
        weight += token.weight();
    }
    return weight;
}

// Σ weight × (1 − best possible score) over the tokens of `own`: the least weight align() must leave unmatched.
// Stops as soon as it exceeds `budget`.
double lostWeight(std::span<const AlignToken> own, std::span<const AlignToken> other, double budget)
{
    own = aligned(own);
    other = aligned(other);
    double lost = 0.0;
    const auto n = static_cast<qsizetype>(own.size());
    for (qsizetype i = 0; i < n && lost <= budget; ++i) {
        const double best = other.empty() ? 0.0 : bestPossible(own, i, other);
        lost += own[i].weight() * (1.0 - best);
    }
    return lost;
}

// Rounding slack for the pruning tests, relative to the weights: far above double rounding, far below any real
// difference (weights are whole code-point counts).
constexpr double kBoundSlack = 1e-6;

// Candidate pairs stay on the stack: fields are short (at most kMaxFieldTokens tokens a side).
using Candidates = QVarLengthArray<Candidate, 2 * kMaxFieldTokens>;

Candidates candidates(std::span<const AlignToken> a, std::span<const AlignToken> b)
{
    Candidates result;
    const auto n = static_cast<qsizetype>(a.size());
    const auto m = static_cast<qsizetype>(b.size());
    for (qsizetype i = 0; i < n; ++i) {
        for (qsizetype j = 0; j < m; ++j) {
            if (!mayMatch(a[i], b[j])) {
                continue; // most pairs: no comparison needed
            }
            const TokenComparison comparison = TokenRules::compare(a[i].text, b[j].text);
            if (comparison.matched()) {
                result.append(
                    {{i, -1, j, -1, comparison}, a[i].facts.length + b[j].facts.length, a[i].text, b[j].text});
            }
        }
    }
    // Joined pairs: two adjacent tokens of one side equal one token of the other, exactly (§17.4 step 3).
    const TokenComparison joined{TokenMatchKind::Joined, 0, false, false};
    for (qsizetype i = 0; i + 1 < n; ++i) {
        if (a[i].segment != a[i + 1].segment) {
            continue;
        }
        for (qsizetype j = 0; j < m; ++j) {
            if (equalsJoined(b[j].text, a[i].text, a[i + 1].text)) {
                result.append({{i, i + 1, j, -1, joined}, 2 * b[j].facts.length, a[i].text, b[j].text});
            }
        }
    }
    for (qsizetype j = 0; j + 1 < m; ++j) {
        if (b[j].segment != b[j + 1].segment) {
            continue;
        }
        for (qsizetype i = 0; i < n; ++i) {
            if (equalsJoined(a[i].text, b[j].text, b[j + 1].text)) {
                result.append({{i, -1, j, j + 1, joined}, 2 * a[i].facts.length, a[i].text, b[j].text});
            }
        }
    }
    return result;
}

// The symmetric part of the order: fewest edits, most characters covered, then the token texts.
auto symmetricKey(const Candidate &c)
{
    const QStringView low = std::min(c.textA, c.textB);
    const QStringView high = std::max(c.textA, c.textB);
    return std::make_tuple(c.pair.comparison.edits, -c.covered, low, high);
}

struct Selection
{
    Candidates chosen;
    bool conflicts = false; // a candidate was refused because one of its tokens was taken
};

Selection greedy(Candidates list, qsizetype n, qsizetype m, bool aFirst)
{
    // A total order (the indexes break every tie), so an unstable sort is deterministic.
    std::ranges::sort(list, [aFirst](const Candidate &x, const Candidate &y) {
        const auto kx = symmetricKey(x);
        const auto ky = symmetricKey(y);
        if (kx != ky) {
            return kx < ky;
        }
        return aFirst ? std::tie(x.pair.a, x.pair.aSecond, x.pair.b, x.pair.bSecond) <
                            std::tie(y.pair.a, y.pair.aSecond, y.pair.b, y.pair.bSecond)
                      : std::tie(x.pair.b, x.pair.bSecond, x.pair.a, x.pair.aSecond) <
                            std::tie(y.pair.b, y.pair.bSecond, y.pair.a, y.pair.aSecond);
    });
    QVarLengthArray<bool, kMaxFieldTokens> usedA(n, false);
    QVarLengthArray<bool, kMaxFieldTokens> usedB(m, false);
    Selection selection;
    for (const Candidate &c : std::as_const(list)) {
        const TokenPair &p = c.pair;
        const bool free =
            !usedA[p.a] && (p.aSecond < 0 || !usedA[p.aSecond]) && !usedB[p.b] && (p.bSecond < 0 || !usedB[p.bSecond]);
        if (!free) {
            selection.conflicts = true;
            continue;
        }
        usedA[p.a] = true;
        usedB[p.b] = true;
        if (p.aSecond >= 0) {
            usedA[p.aSecond] = true;
        }
        if (p.bSecond >= 0) {
            usedB[p.bSecond] = true;
        }
        selection.chosen.append(c);
    }
    return selection;
}

int squashedLength(std::span<const AlignToken> tokens)
{
    int length = 0;
    for (const AlignToken &token : tokens) {
        length += token.facts.length;
    }
    return length;
}

// The worse of two fuzzy pairs: more edits, fewer characters, then by text (symmetric).
bool worse(const Candidate &x, const Candidate &y)
{
    return std::make_tuple(x.pair.comparison.edits, -x.covered, std::min(x.textA, x.textB),
                           std::max(x.textA, x.textB)) <
           std::make_tuple(y.pair.comparison.edits, -y.covered, std::min(y.textA, y.textB), std::max(y.textA, y.textB));
}

void dropWorstFuzzy(Candidates &chosen)
{
    qsizetype worst = -1;
    for (qsizetype k = 0; k < chosen.size(); ++k) {
        if (chosen[k].pair.comparison.edits > 0 && (worst < 0 || worse(chosen[worst], chosen[k]))) {
            worst = k;
        }
    }
    if (worst >= 0) {
        chosen.remove(worst);
    }
}

// Field conditions of §17.3 on one greedy selection.
void applyFieldConditions(Candidates &chosen, qsizetype n, qsizetype m, int fieldBudget, FieldMatchOptions options)
{
    const bool corroborated = options.durationCorroborates || std::ranges::any_of(chosen, [](const Candidate &c) {
                                  return isExactKind(c.pair.comparison.kind);
                              });
    if (!corroborated) {
        chosen.removeIf([](const Candidate &c) { return c.pair.comparison.needsCorroboration; });
    }
    const auto fuzzyCount = [&chosen] {
        return std::ranges::count_if(chosen, [](const Candidate &c) { return c.pair.comparison.edits > 0; });
    };
    if (std::min(n, m) <= 2) {
        while (fuzzyCount() > 1) {
            dropWorstFuzzy(chosen);
        }
    }
    const auto totalEdits = [&chosen] {
        int edits = 0;
        for (const Candidate &c : std::as_const(chosen)) {
            edits += c.pair.comparison.edits;
        }
        return edits;
    };
    while (totalEdits() > fieldBudget) {
        dropWorstFuzzy(chosen);
    }
}

Alignment score(const Candidates &chosen, std::span<const AlignToken> a, std::span<const AlignToken> b)
{
    Alignment result;
    result.scoreA.resize(static_cast<qsizetype>(a.size()));
    result.scoreB.resize(static_cast<qsizetype>(b.size()));
    std::ranges::fill(result.scoreA, 0.0);
    std::ranges::fill(result.scoreB, 0.0);
    for (const Candidate &c : chosen) {
        const double s = c.pair.comparison.score();
        result.scoreA[c.pair.a] = s;
        result.scoreB[c.pair.b] = s;
        if (c.pair.aSecond >= 0) {
            result.scoreA[c.pair.aSecond] = s;
        }
        if (c.pair.bSecond >= 0) {
            result.scoreB[c.pair.bSecond] = s;
        }
        result.edits += c.pair.comparison.edits;
        result.pairs.append(c.pair);
    }
    result.fuzzy = result.edits > 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double w = a[i].weight();
        result.weightA += w;
        result.matchedA += w * result.scoreA[static_cast<qsizetype>(i)];
    }
    for (std::size_t j = 0; j < b.size(); ++j) {
        const double w = b[j].weight();
        result.weightB += w;
        result.matchedB += w * result.scoreB[static_cast<qsizetype>(j)];
    }
    return result;
}

} // namespace

TokenFacts tokenFacts(QStringView token)
{
    TokenFacts facts;
    facts.length = static_cast<quint16>(std::min<qsizetype>(TokenRules::length(token), 0xFFFF));
    facts.light = lexicon::isLightWord(token);
    facts.digits = std::ranges::any_of(token, [](QChar c) { return c.isDigit(); });
    return facts;
}

AlignToken alignToken(QStringView text, int segment, bool ignorable)
{
    return {text, static_cast<qint16>(segment), ignorable, tokenFacts(text)};
}

Alignment align(std::span<const AlignToken> a, std::span<const AlignToken> b, FieldMatchOptions options)
{
    a = a.first(std::min<std::size_t>(a.size(), kMaxFieldTokens));
    b = b.first(std::min<std::size_t>(b.size(), kMaxFieldTokens));
    const auto n = static_cast<qsizetype>(a.size());
    const auto m = static_cast<qsizetype>(b.size());
    if (n == 0 || m == 0) {
        return score({}, a, b);
    }
    const int fieldBudget = TokenRules::fieldEditBudget(std::min(squashedLength(a), squashedLength(b)));
    const Candidates all = candidates(a, b);

    Selection forward = greedy(all, n, m, /*aFirst=*/true);
    const bool conflicts = forward.conflicts;
    applyFieldConditions(forward.chosen, n, m, fieldBudget, options);
    Alignment best = score(forward.chosen, a, b);
    if (!conflicts) {
        return best; // every candidate was taken: the other direction would take the same ones
    }
    Selection backward = greedy(all, n, m, /*aFirst=*/false);
    applyFieldConditions(backward.chosen, n, m, fieldBudget, options);
    Alignment other = score(backward.chosen, a, b);
    if (other.similarity() > best.similarity()) {
        best = std::move(other);
    }
    return best;
}

void appendAlignTokens(AlignTokens &out, QStringView key, int segment, bool ignorable)
{
    qsizetype start = -1;
    for (qsizetype i = 0; i <= key.size(); ++i) {
        const bool space = i == key.size() || key[i] == u' ';
        if (space && start >= 0) {
            out.append(alignToken(key.sliced(start, i - start), segment, ignorable));
            start = -1;
        } else if (!space && start < 0) {
            start = i;
        }
    }
}

FieldView fieldView(const FieldKey &key)
{
    FieldView view;
    appendAlignTokens(view.tokens, key.key);
    view.squashed = key.squashed;
    view.leet = key.leet;
    return view;
}

bool coverageMayReach(std::span<const AlignToken> a, std::span<const AlignToken> b, double threshold)
{
    const double weight = totalWeight(a);
    if (weight <= 0.0) {
        return threshold <= 0.0; // coverage 0
    }
    // coverage ≤ (weight − lost) / weight, so it reaches the threshold only while lost ≤ (1 − threshold) × weight.
    const double budget = ((1.0 - threshold) * weight) + (kBoundSlack * weight);
    return lostWeight(a, b, budget) <= budget;
}

bool similarityMayReach(const FieldView &a, const FieldView &b, double threshold)
{
    if (a.isEmpty() || b.isEmpty()) {
        return threshold <= 0.0;
    }
    const QStringView leetA = a.leet.isEmpty() ? a.squashed : a.leet;
    const QStringView leetB = b.leet.isEmpty() ? b.squashed : b.leet;
    if ((!a.squashed.isEmpty() && a.squashed == b.squashed) || (!leetA.isEmpty() && leetA == leetB)) {
        return true;
    }
    const std::span<const AlignToken> tokensA(a.tokens.data(), static_cast<std::size_t>(a.tokens.size()));
    const std::span<const AlignToken> tokensB(b.tokens.data(), static_cast<std::size_t>(b.tokens.size()));
    const double weight = totalWeight(tokensA) + totalWeight(tokensB);
    const double budget = ((1.0 - threshold) * weight) + (kBoundSlack * weight);
    double lost = lostWeight(tokensA, tokensB, budget);
    if (lost <= budget) {
        lost += lostWeight(tokensB, tokensA, budget - lost);
        if (lost <= budget) {
            return true;
        }
    }
    // The squashed keys as one token: no match is possible beyond 2 units apart (OSA over units is at least the gap,
    // a leet match keeps the length); otherwise TokenRules::compare is cheap when it fails (a bit-parallel OSA).
    const bool close =
        !a.squashed.isEmpty() && !b.squashed.isEmpty() && std::abs(a.squashed.size() - b.squashed.size()) <= 2;
    return close && TokenRules::compare(a.squashed, b.squashed).score() >= threshold - kBoundSlack;
}

} // namespace selecta::match::detail
