// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QStringView>

#include <utility>

// Internal: small text scanners shared by the normaliser, the version classifier and the parser. Pure functions; any
// thread. Indexes are UTF-16 code units.
namespace selecta::match::detail {

// The code point at `i` and its width (1, or 2 for a valid surrogate pair; a lone surrogate is returned as is).
std::pair<char32_t, qsizetype> codePointAt(QStringView text, qsizetype i);

// The first index ≥ `from` of any character of `chars`, or -1.
qsizetype indexOfAny(QStringView text, QStringView chars, qsizetype from);

bool isOpenBracket(QChar c);
bool isCloseBracket(QChar c);

// The bracket closing the one at `open` ("(", "[" or "{"), counting nesting across bracket kinds; -1 if unbalanced.
qsizetype matchingBracket(QStringView text, qsizetype open);

// The bracket opening the one at `close`, counting nesting; -1 if unbalanced.
qsizetype matchingOpenBracket(QStringView text, qsizetype close);

// The length of a leading featured-artist marker ("featuring", "feat.", "feat", "ft.", "ft", and "with" inside
// brackets) with the spaces after it, when it is a whole word followed by a name; otherwise 0. Case-insensitive.
qsizetype featMarkerLength(QStringView text, bool inBrackets);

// The index of the space before an unbracketed " feat. ", " ft. " or " featuring " marker that is followed by a
// name, or -1.
qsizetype findFeatMarker(QStringView text);

// A four-digit year 1900–2099.
bool isYearToken(QStringView token);

// Whitespace-separated words, as views into `text`.
QList<QStringView> words(QStringView text);

// Every code unit is an ASCII digit (and there is at least one).
bool isDigits(QStringView text);

} // namespace selecta::match::detail
