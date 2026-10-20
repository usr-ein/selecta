// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/Query.h"

#include "governor/Limits.h"
#include "share/detail/Text.h"

#include <algorithm>

namespace selecta::share::detail {

namespace {

constexpr qsizetype kMinPlainWordCodePoints = governor::limits::kAnswerMinChars;

void addTerm(QStringView term, ParsedQuery &parsed)
{
    QStringList *target = &parsed.plain;
    if (term.startsWith(u'-')) {
        target = &parsed.excluded;
        term = term.sliced(1);
    } else if (term.startsWith(u'*')) {
        target = &parsed.partial;
        term = term.sliced(1);
    }
    const QStringList words = searchWords(term);
    for (const QString &word : words) {
        if (!target->contains(word)) {
            target->append(word);
        }
    }
}

} // namespace

bool ParsedQuery::isAnswerable() const
{
    return std::ranges::any_of(plain,
                               [](const QString &word) { return codePointCount(word) >= kMinPlainWordCodePoints; });
}

ParsedQuery parseQuery(QStringView query)
{
    ParsedQuery parsed;
    qsizetype start = -1;
    for (qsizetype i = 0; i <= query.size(); ++i) {
        const bool space = i == query.size() || query[i].isSpace();
        if (space && start >= 0) {
            addTerm(query.sliced(start, i - start), parsed);
            start = -1;
        } else if (!space && start < 0) {
            start = i;
        }
    }
    return parsed;
}

} // namespace selecta::share::detail
