// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Decision.h"

#include <QString>
#include <QStringList>

namespace selecta::governor {

// Cleans a search before it goes on the network (etiquette note §9.1, "Query hygiene"):
// - spaces are collapsed and trimmed;
// - one-character words are dropped (peers ignore them, and they only widen the match);
// - fewer than 3 or more than 256 characters is refused;
// - a query containing a phrase the server excludes is refused, never evaded (no wildcard rewriting).
// Pure and thread-safe.
struct CleanQuery
{
    QString query;                   // what to send; empty when refused
    Refusal refusal = Refusal::None; // why it can't be sent
    QString excludedPhrase;          // the offending phrase, for QueryExcludedPhrase
};

CleanQuery cleanQuery(const QString &raw, const QStringList &excludedPhrases);

// The key used to recognise "the same search": cleaned, case-folded, words sorted.
QString queryKey(const QString &cleanedQuery);

} // namespace selecta::governor
