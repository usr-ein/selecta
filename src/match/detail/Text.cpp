// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/Text.h"

#include <QChar>

#include <algorithm>
#include <array>

namespace selecta::match::detail {

std::pair<char32_t, qsizetype> codePointAt(QStringView text, qsizetype i)
{
    const QChar c = text[i];
    if (c.isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
        return {QChar::surrogateToUcs4(c, text[i + 1]), 2};
    }
    return {c.unicode(), 1};
}

qsizetype indexOfAny(QStringView text, QStringView chars, qsizetype from)
{
    for (qsizetype i = std::max<qsizetype>(from, 0); i < text.size(); ++i) {
        if (chars.contains(text[i])) {
            return i;
        }
    }
    return -1;
}

bool isOpenBracket(QChar c)
{
    return c == u'(' || c == u'[' || c == u'{';
}

bool isCloseBracket(QChar c)
{
    return c == u')' || c == u']' || c == u'}';
}

qsizetype matchingBracket(QStringView text, qsizetype open)
{
    int depth = 0;
    for (qsizetype i = open; i < text.size(); ++i) {
        if (isOpenBracket(text[i])) {
            ++depth;
        } else if (isCloseBracket(text[i])) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return -1;
}

qsizetype matchingOpenBracket(QStringView text, qsizetype close)
{
    int depth = 0;
    for (qsizetype i = close; i >= 0; --i) {
        if (isCloseBracket(text[i])) {
            ++depth;
        } else if (isOpenBracket(text[i])) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return -1;
}

qsizetype featMarkerLength(QStringView text, bool inBrackets)
{
    static constexpr std::array<QStringView, 5> kMarkers{u"featuring", u"feat.", u"feat", u"ft.", u"ft"};
    QStringView found;
    for (const QStringView marker : kMarkers) {
        if (text.startsWith(marker, Qt::CaseInsensitive)) {
            found = marker;
            break;
        }
    }
    if (found.isEmpty() && inBrackets && text.startsWith(u"with", Qt::CaseInsensitive)) {
        found = u"with";
    }
    if (found.isEmpty()) {
        return 0;
    }
    qsizetype length = found.size();
    // A whole word: "feat." already ends at a boundary; "feat", "ft" and "with" must be followed by a space or a dot.
    if (!found.endsWith(u'.')) {
        if (length < text.size() && text[length] == u'.') {
            ++length;
        } else if (length < text.size() && !text[length].isSpace()) {
            return 0;
        }
    }
    while (length < text.size() && text[length].isSpace()) {
        ++length;
    }
    // A name must follow.
    return length < text.size() ? length : 0;
}

qsizetype findFeatMarker(QStringView text)
{
    for (qsizetype i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == u' ' && featMarkerLength(text.sliced(i + 1), /*inBrackets=*/false) > 0) {
            return i;
        }
    }
    return -1;
}

bool isYearToken(QStringView token)
{
    return token.size() == 4 && isDigits(token) && (token.startsWith(u"19") || token.startsWith(u"20"));
}

QList<QStringView> words(QStringView text)
{
    QList<QStringView> result;
    qsizetype start = -1;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        const bool space = i == text.size() || text[i].isSpace();
        if (space && start >= 0) {
            result.append(text.sliced(start, i - start));
            start = -1;
        } else if (!space && start < 0) {
            start = i;
        }
    }
    return result;
}

bool isDigits(QStringView text)
{
    return !text.isEmpty() && std::ranges::all_of(text, [](QChar c) { return c >= u'0' && c <= u'9'; });
}

} // namespace selecta::match::detail
