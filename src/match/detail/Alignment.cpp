// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/Alignment.h"

#include "match/Lexicon.h"

#include <QString>

#include <algorithm>
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

int lengthOf(QStringView token)
{
    return static_cast<int>(TokenRules::length(token));
}

// `whole` equals `first` followed by `second`, without building the joined string.
bool equalsJoined(QStringView whole, QStringView first, QStringView second)
{
    return whole.size() == first.size() + second.size() && whole.startsWith(first) &&
           whole.sliced(first.size()) == second;
}

QList<Candidate> candidates(std::span<const AlignToken> a, std::span<const AlignToken> b)
{
    QList<Candidate> result;
    const auto n = static_cast<qsizetype>(a.size());
    const auto m = static_cast<qsizetype>(b.size());
    for (qsizetype i = 0; i < n; ++i) {
        for (qsizetype j = 0; j < m; ++j) {
            const TokenComparison comparison = TokenRules::compare(a[i].text, b[j].text);
            if (comparison.matched()) {
                result.append(
                    {{i, -1, j, -1, comparison}, lengthOf(a[i].text) + lengthOf(b[j].text), a[i].text, b[j].text});
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
                result.append({{i, i + 1, j, -1, joined}, 2 * lengthOf(b[j].text), a[i].text, b[j].text});
            }
        }
    }
    for (qsizetype j = 0; j + 1 < m; ++j) {
        if (b[j].segment != b[j + 1].segment) {
            continue;
        }
        for (qsizetype i = 0; i < n; ++i) {
            if (equalsJoined(a[i].text, b[j].text, b[j + 1].text)) {
                result.append({{i, -1, j, j + 1, joined}, 2 * lengthOf(a[i].text), a[i].text, b[j].text});
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

QList<Candidate> greedy(QList<Candidate> list, qsizetype n, qsizetype m, bool aFirst)
{
    std::ranges::stable_sort(list, [aFirst](const Candidate &x, const Candidate &y) {
        const auto kx = symmetricKey(x);
        const auto ky = symmetricKey(y);
        if (kx != ky) {
            return kx < ky;
        }
        return aFirst ? std::tie(x.pair.a, x.pair.b) < std::tie(y.pair.a, y.pair.b)
                      : std::tie(x.pair.b, x.pair.a) < std::tie(y.pair.b, y.pair.a);
    });
    QList<bool> usedA(n, false);
    QList<bool> usedB(m, false);
    QList<Candidate> chosen;
    for (const Candidate &c : std::as_const(list)) {
        const TokenPair &p = c.pair;
        const bool free = !usedA.at(p.a) && (p.aSecond < 0 || !usedA.at(p.aSecond)) && !usedB.at(p.b) &&
                          (p.bSecond < 0 || !usedB.at(p.bSecond));
        if (!free) {
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
        chosen.append(c);
    }
    return chosen;
}

int squashedLength(std::span<const AlignToken> tokens)
{
    int length = 0;
    for (const AlignToken &token : tokens) {
        length += lengthOf(token.text);
    }
    return length;
}

// Field conditions of §17.3 on one greedy selection.
void applyFieldConditions(QList<Candidate> &chosen, qsizetype n, qsizetype m, int fieldBudget,
                          FieldMatchOptions options)
{
    const bool corroborated = options.durationCorroborates || std::ranges::any_of(chosen, [](const Candidate &c) {
                                  return isExactKind(c.pair.comparison.kind);
                              });
    if (!corroborated) {
        chosen.removeIf([](const Candidate &c) { return c.pair.comparison.needsCorroboration; });
    }
    const auto isFuzzy = [](const Candidate &c) { return c.pair.comparison.edits > 0; };
    // The worst fuzzy pair first: most edits, fewest characters, then by text (symmetric).
    const auto worse = [](const Candidate &x, const Candidate &y) {
        return std::make_tuple(x.pair.comparison.edits, -x.covered, std::min(x.textA, x.textB),
                               std::max(x.textA, x.textB)) < std::make_tuple(y.pair.comparison.edits, -y.covered,
                                                                             std::min(y.textA, y.textB),
                                                                             std::max(y.textA, y.textB));
    };
    const auto dropWorstFuzzy = [&chosen, &isFuzzy, &worse] {
        qsizetype worst = -1;
        for (qsizetype k = 0; k < chosen.size(); ++k) {
            if (isFuzzy(chosen.at(k)) && (worst < 0 || worse(chosen.at(worst), chosen.at(k)))) {
                worst = k;
            }
        }
        if (worst >= 0) {
            chosen.removeAt(worst);
        }
    };
    if (std::min(n, m) <= 2) {
        while (std::ranges::count_if(chosen, isFuzzy) > 1) {
            dropWorstFuzzy();
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
        dropWorstFuzzy();
    }
}

Alignment score(const QList<Candidate> &chosen, std::span<const AlignToken> a, std::span<const AlignToken> b)
{
    Alignment result;
    result.scoreA = QList<double>(static_cast<qsizetype>(a.size()), 0.0);
    result.scoreB = QList<double>(static_cast<qsizetype>(b.size()), 0.0);
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
        const double w = tokenWeight(a[i].text);
        result.weightA += w;
        result.matchedA += w * result.scoreA.at(static_cast<qsizetype>(i));
    }
    for (std::size_t j = 0; j < b.size(); ++j) {
        const double w = tokenWeight(b[j].text);
        result.weightB += w;
        result.matchedB += w * result.scoreB.at(static_cast<qsizetype>(j));
    }
    return result;
}

} // namespace

double tokenWeight(QStringView token)
{
    return lexicon::isLightWord(token) ? 1.0 : static_cast<double>(TokenRules::length(token));
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
    const QList<Candidate> all = candidates(a, b);

    QList<Candidate> forward = greedy(all, n, m, /*aFirst=*/true);
    applyFieldConditions(forward, n, m, fieldBudget, options);
    Alignment best = score(forward, a, b);

    QList<Candidate> backward = greedy(all, n, m, /*aFirst=*/false);
    applyFieldConditions(backward, n, m, fieldBudget, options);
    Alignment other = score(backward, a, b);
    if (other.similarity() > best.similarity()) {
        best = std::move(other);
    }
    return best;
}

QList<AlignToken> alignTokens(QStringView key, int segment)
{
    QList<AlignToken> tokens;
    qsizetype start = -1;
    for (qsizetype i = 0; i <= key.size(); ++i) {
        const bool space = i == key.size() || key[i] == u' ';
        if (space && start >= 0) {
            tokens.append({key.sliced(start, i - start), segment, false});
            start = -1;
        } else if (!space && start < 0) {
            start = i;
        }
    }
    return tokens;
}

QString digitsOf(QStringView text)
{
    QString digits;
    for (const QChar c : text) {
        if (c.isDigit()) {
            digits.append(c);
        }
    }
    return digits;
}

} // namespace selecta::match::detail
