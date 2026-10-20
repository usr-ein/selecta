// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Normalizer.h"

#include <QList>
#include <QString>
#include <QStringView>

namespace selecta::match {

// Typo tolerance (note §17.3–17.4, PRD D28, ADR-008). Tokens are normalised words (Normalizer::tokens).
inline constexpr double kFuzzyEditPenalty = 0.15;     // a fuzzy token scores 1 − 0.15 × edits: typos never look exact
inline constexpr double kSquashedRatioMin = 85.0;     // fuzz::ratio gate of the whole-field route on squashed keys
inline constexpr qsizetype kMaxFieldTokens = 24;      // tokens beyond this are ignored (bounded alignment work)
inline constexpr qsizetype kMaxFuzzyTokenLength = 48; // longer tokens compare exactly only

enum class TokenMatchKind : quint8 {
    None,
    Exact,    // equal tokens
    Leet,     // equal after leet folding of a token that mixes letters and digits ("deadmau5" = "deadmaus")
    Joined,   // two adjacent tokens of one side equal one token of the other ("okuma"+"drift" = "okumadrift")
    Skeleton, // equal after collapsing doubled letters, tokens of ≥ 5 characters ("gallaxy" = "galaxy"): one edit
    Fuzzy,    // within the OSA budget of §17.3
};

// The comparison of two tokens on their own, before the field-level conditions are applied.
struct TokenComparison
{
    TokenMatchKind kind = TokenMatchKind::None;
    int edits = 0;                   // OSA edits (Skeleton: 1; Exact, Leet, Joined: 0)
    bool substitution = false;       // the cheapest edit script needs a substitution
    bool needsCorroboration = false; // 4–5 characters, or a substitution at 6–8: only valid with corroboration

    bool matched() const { return kind != TokenMatchKind::None; }
    double score() const { return matched() ? 1.0 - (kFuzzyEditPenalty * edits) : 0.0; }
};

// Per-token edit rules (note §17.3). Lengths count Unicode code points.
//
//   ≤ 3 characters, or any digit         exact only (leet folding aside); "dj", "mk", "808", "1999"
//   4–5 characters                       1 insertion, deletion or adjacent transposition; no substitution; same first
//                                        letter; needs corroboration
//   6–8 characters                       1 OSA edit; same first letter unless the edit swaps the first two letters; a
//                                        substitution needs corroboration
//   ≥ 9 characters                       2 OSA edits, at most one of them a substitution
//
// The budget of a pair is that of the shorter token. Threading: pure; any thread.
class TokenRules final
{
public:
    // The edit budget of a token on its own (0, 1 or 2).
    static int editBudget(QStringView token);

    // At most 1 edit in a field of ≤ 12 characters (squashed), 2 above.
    static int fieldEditBudget(qsizetype squashedLength);

    // Compares two tokens. Exact, Leet, Skeleton or Fuzzy; never Joined (that needs the neighbours, FieldMatcher).
    static TokenComparison compare(QStringView a, QStringView b);

    // Doubled letters collapsed: "gallaxy" → "galaxy". Only used for tokens of ≥ 5 characters ("too" ≠ "to").
    static QString skeleton(QStringView token);

    // Leet folding of one normalised token that mixes letters and digits (0→o 1→i 3→e 4→a 5→s 7→t); other tokens are
    // returned unchanged.
    static QString leetFold(QStringView token);

    // Number of Unicode code points.
    static qsizetype length(QStringView token);
};

// One aligned pair of a FieldMatch. Indexes are token positions in the two keys; second indexes are set for Joined.
struct TokenPair
{
    qsizetype a = -1;
    qsizetype aSecond = -1;
    qsizetype b = -1;
    qsizetype bSecond = -1;
    TokenComparison comparison;
};

struct FieldMatchOptions
{
    bool durationCorroborates = false; // the lengths agree within ±3 s: corroborates short-token edits (§17.3)
};

// The alignment of two fields (note §17.4).
struct FieldMatch
{
    enum class Route : quint8 {
        None,
        Tokens,        // token alignment
        Squashed,      // equal squashed keys ("dead mau5" = "deadmau5")
        Leet,          // equal leet keys ("P!nk" = "Pink")
        SquashedFuzzy, // squashed keys within the field budget and fuzz::ratio ≥ 85, treated as one token
    };

    double similarity = 0.0;   // symmetric: (Σ_A w·s + Σ_B w·s) / (Σ_A w + Σ_B w), w = token length (stop words 1)
    double coverageA = 0.0;    // Σ_A w·s / Σ_A w: how much of A is explained by B
    double coverageB = 0.0;    // Σ_B w·s / Σ_B w
    double unexplainedB = 0.0; // characters of B's tokens not used by the alignment / characters of B (u, §17.4)
    int edits = 0;             // edits used by the accepted fuzzy pairs or by the fuzzy squashed route
    bool exact = false;        // similarity 1 with no edits
    bool fuzzy = false;        // an edit (fuzzy or skeleton token, or the fuzzy squashed route) contributed
    Route route = Route::None;
    QList<TokenPair> pairs; // the accepted token pairs (route Tokens)
};

// Field similarity (note §17.4): tokens are aligned one by one, each within its own budget, so reordering and extra
// words are explicit; the squashed and leet keys are alternative whole-field routes.
//
// Alignment: every token pair (and every adjacent pair of either side joined, compared exactly) is a candidate; the
// best candidates are taken greedily (fewest edits, then most characters covered), each token used once; then
// fuzzy pairs that need corroboration are dropped unless another token of the field matched exactly or
// options.durationCorroborates; then at most one fuzzy token is kept in a field of one or two tokens, and fuzzy pairs
// are dropped, worst first, until the field budget (TokenRules::fieldEditBudget) holds. The result is the better of
// the two greedy directions, so compare(a, b) and compare(b, a) give the same similarity.
//
// Threading: pure; any thread. Failure modes: none; empty fields give similarity 0. Only the first kMaxFieldTokens
// tokens of each field are aligned.
class FieldMatcher final
{
public:
    static FieldMatch compare(const FieldKey &a, const FieldKey &b, FieldMatchOptions options = {});

    // As compare(), from normalised keys only (no leet route).
    static FieldMatch compareKeys(QStringView a, QStringView b, FieldMatchOptions options = {});
};

} // namespace selecta::match
