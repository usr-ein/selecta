// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/TextFold.h"

using namespace Qt::StringLiterals;

namespace selecta::meta::detail {

namespace {

constexpr qsizetype kMaxInput = 1000;

bool isApostrophe(QChar c)
{
    return c == u'\'' || c == u'’' || c == u'‘' || c == u'ʼ' || c == u'`' || c == u'´';
}

bool isMark(QChar c)
{
    const QChar::Category category = c.category();
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
           category == QChar::Mark_Enclosing;
}

// Letters that don't decompose under NFKD, after case folding.
QStringView transliterated(QChar c)
{
    switch (c.unicode()) {
    case 0x00F8: // ø
        return u"o";
    case 0x00E6: // æ
        return u"ae";
    case 0x0153: // œ
        return u"oe";
    case 0x00DF: // ß
        return u"ss";
    case 0x0142: // ł
        return u"l";
    case 0x0111: // đ
    case 0x00F0: // ð
        return u"d";
    case 0x00FE: // þ
        return u"th";
    case 0x0131: // ı
        return u"i";
    default:
        return {};
    }
}

// A dot that only separates single letters ("A.T.F.C.") disappears instead of splitting the word.
bool dotBetweenSingleLetters(const QString &text, qsizetype i)
{
    const bool letterBefore = i >= 1 && text.at(i - 1).isLetter();
    const bool singleBefore = letterBefore && (i == 1 || !text.at(i - 2).isLetterOrNumber());
    const bool letterAfterOrEnd = i + 1 >= text.size() || text.at(i + 1).isLetter() || text.at(i + 1).isSpace();
    return singleBefore && letterAfterOrEnd;
}

} // namespace

QString foldKey(const QString &input)
{
    const QString text = input.left(kMaxInput).normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (isMark(c) || isApostrophe(c)) {
            continue;
        }
        if (c == u'&' || c == u'+') {
            out += u" and "_s;
            continue;
        }
        if (c == u'$') {
            out += u's';
            continue;
        }
        if (c == u'.' && dotBetweenSingleLetters(text, i)) {
            continue;
        }
        if (c.isSurrogate() || !c.isLetterOrNumber()) {
            out += u' ';
            continue;
        }
        const QChar folded = c.toCaseFolded();
        const QStringView replacement = transliterated(folded);
        if (replacement.isEmpty()) {
            out += folded;
        } else {
            out += replacement;
        }
    }
    return out.simplified();
}

QString squash(const QString &key)
{
    QString out = key;
    out.remove(u' ');
    return out;
}

} // namespace selecta::meta::detail
