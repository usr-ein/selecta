// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/ReleaseScorer.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace selecta::match {

// Network query words (note §17.7; network etiquette note §4.3, §7.3, §9.2).
inline constexpr qsizetype kMinQueryWordLength = 3; // responders ignore shorter words and queries
inline constexpr qsizetype kMaxQueryWords = 5;      // every extra word is one more chance to miss a spelling variant
inline constexpr qsizetype kMaxQueryLength = 256;   // servers drop longer queries

// The default query and the "Search wider" ladder. Each step is one explicit user click, sent as one budgeted search
// by the caller through the governor; the planner never sends anything and never fans out.
enum class QueryStep : quint8 {
    Default,         // the release's artist and title words, spelled as the release
    SpellingVariant, // diacritics folded ("royksopp"), a multi-word artist joined ("okumadrift"): never a wildcard
    FewerWords,      // without the least reliable words (symbols, the longest word): typically the title words
    ArtistOnly,      // the artist's words, for short or generic titles
};

enum class QueryStatus : quint8 {
    Ready,     // send `text`
    Blocked,   // a word contains an excluded phrase: warn, send nothing, spend no budget (T84)
    NoWords,   // nothing selective is left (every word too short or a stop word)
    Unchanged, // this wider step gives the same query as an earlier step: nothing new to send
};

struct QueryPlan
{
    QueryStep step = QueryStep::Default;
    QueryStatus status = QueryStatus::NoWords;
    QStringList words;      // lower case, in release order
    QString text;           // words joined by single spaces
    QString blockedPhrase;  // the excluded phrase that blocked the query
    bool respelled = false; // the typed text is spelled differently from the release (SRCH-19: the summary says so)
    QStringList notes;      // why words were dropped or changed, for the UI

    bool isReady() const { return status == QueryStatus::Ready; }
};

// Plans network queries from a release (note §17.7). Words are split the way responders split shared paths
// (Nicotine+'s punctuation set, so "Ke$ha" is "ke" + "ha" and "Don't" is "don" + "t"), case-folded but otherwise
// spelled as the release; words under 3 characters, stop and version words, and both fragments of an apostrophe or
// symbol split are dropped; words with diacritics, apostrophes or symbols are left out when the rest stays selective
// (≥ 2 words of ≥ 4 characters, or one of ≥ 6); at most 5 words, keeping the longest; the remixer's words join the
// query for remix versions. No wildcards, ever.
//
// Threading: immutable; plan() may be called from any thread. Failure modes: none; see QueryStatus.
class QueryPlanner final
{
public:
    // `excludedPhrases` is the list the server sends at login (ExcludedSearchPhrases, code 160).
    explicit QueryPlanner(QStringList excludedPhrases = {});

    // One step's query. `typed` is what the user typed, only used to report a respelling.
    QueryPlan plan(const ReleaseReference &release, QueryStep step = QueryStep::Default, QStringView typed = {}) const;

    // The wider steps after the default query, in order; a step that repeats an earlier query is Unchanged.
    QList<QueryPlan> widerLadder(const ReleaseReference &release) const;

    // The words a responder indexes for `text`: split on Nicotine+'s punctuation set and whitespace, case-folded.
    static QStringList responderWords(QStringView text);

private:
    QStringList m_excludedPhrases; // case-folded
};

} // namespace selecta::match
