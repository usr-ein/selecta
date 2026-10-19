// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/QueryHygiene.h"

#include "governor/Limits.h"

#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::governor {

CleanQuery cleanQuery(const QString &raw, const QStringList &excludedPhrases)
{
    CleanQuery result;
    const QStringList words = raw.simplified().split(u' ', Qt::SkipEmptyParts);
    QStringList kept;
    kept.reserve(words.size());
    for (const QString &word : words) {
        // A "-word" exclusion keeps its meaning only when the word itself is long enough.
        const qsizetype length = word.startsWith(u'-') ? word.size() - 1 : word.size();
        if (length > 1) {
            kept.append(word);
        }
    }
    const QString cleaned = kept.join(u' ');
    if (cleaned.size() < limits::kQueryMinChars) {
        result.refusal = Refusal::QueryTooShort;
        return result;
    }
    if (cleaned.size() > limits::kQueryMaxChars) {
        result.refusal = Refusal::QueryTooLong;
        return result;
    }
    const QString folded = cleaned.toCaseFolded();
    for (const QString &phrase : excludedPhrases) {
        const QString needle = phrase.simplified().toCaseFolded();
        if (!needle.isEmpty() && folded.contains(needle)) {
            result.refusal = Refusal::QueryExcludedPhrase;
            result.excludedPhrase = phrase.simplified();
            return result;
        }
    }
    result.query = cleaned;
    return result;
}

QString queryKey(const QString &cleanedQuery)
{
    QStringList words = cleanedQuery.toCaseFolded().split(u' ', Qt::SkipEmptyParts);
    std::ranges::sort(words);
    const auto duplicates = std::ranges::unique(words);
    words.erase(duplicates.begin(), duplicates.end());
    return words.join(u' ');
}

} // namespace selecta::governor
