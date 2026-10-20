// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/Text.h"

#include "match/QueryPlanner.h"
#include "match/RemotePathParser.h"
#include "slsk/Codec.h"

#include <QFile>
#include <QSet>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::share::detail {

namespace {

constexpr qsizetype kMaxAliasChars = 255;

bool isAscii(QStringView text)
{
    return std::ranges::all_of(text, [](QChar c) { return c.unicode() < 0x80; });
}

bool hasControlCharacter(QStringView text)
{
    return std::ranges::any_of(text, [](QChar c) { return c.category() == QChar::Other_Control; });
}

// Case-folded letters that NFKD leaves alone, in their usual Latin spelling.
QStringView transliteration(char32_t c)
{
    switch (c) {
    case U'ø':
        return u"o";
    case U'æ':
        return u"ae";
    case U'œ':
        return u"oe";
    case U'ß':
        return u"ss";
    case U'đ':
    case U'ð':
        return u"d";
    case U'þ':
        return u"th";
    case U'ł':
        return u"l";
    case U'ı':
        return u"i";
    case U'ħ':
        return u"h";
    case U'ŧ':
        return u"t";
    default:
        return {};
    }
}

} // namespace

QString lowerExtension(QStringView name)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot <= 0 || dot + 1 >= name.size()) {
        return {};
    }
    return name.sliced(dot + 1).toString().toLower();
}

FileKind fileKind(QStringView name)
{
    static constexpr std::array<QStringView, 3> kImages{u"jpg", u"jpeg", u"png"};
    static constexpr std::array<QStringView, 5> kCompanions{u"cue", u"log", u"nfo", u"txt", u"pdf"};
    const QString extension = lowerExtension(name);
    if (extension.isEmpty()) {
        return FileKind::Other;
    }
    if (match::RemotePathParser::fileKind(extension) == match::FileKind::Audio) {
        return FileKind::Audio;
    }
    if (std::ranges::find(kImages, QStringView(extension)) != kImages.end()) {
        return FileKind::Image;
    }
    if (std::ranges::find(kCompanions, QStringView(extension)) != kCompanions.end()) {
        return FileKind::Companion;
    }
    return FileKind::Other;
}

bool isHiddenName(QStringView name)
{
    return name.startsWith(u'.');
}

bool isTemporaryName(QStringView name)
{
    static constexpr std::array<QStringView, 8> kSuffixes{u".selecta-part", u".part",       u".partial",  u".tmp",
                                                          u".temp",         u".crdownload", u".download", u".!qB"};
    if (name.startsWith(u"~$") || name.endsWith(u'~')) {
        return true;
    }
    return std::ranges::any_of(kSuffixes,
                               [name](QStringView suffix) { return name.endsWith(suffix, Qt::CaseInsensitive); });
}

bool isJunkFolderName(QStringView name)
{
    static constexpr std::array<QStringView, 7> kJunk{u"$RECYCLE.BIN", u"System Volume Information",
                                                      u"@eaDir",       u"#recycle",
                                                      u"#snapshot",    u"Network Trash Folder",
                                                      u"Temporary Items"};
    return std::ranges::any_of(kJunk, [name](QStringView junk) { return name.compare(junk, Qt::CaseInsensitive) == 0; });
}

LocalName localName(QByteArrayView nativeName)
{
    LocalName result;
    if (!slsk::isValidUtf8(nativeName)) {
        result.problem = u"its name isn't valid UTF-8"_s;
        return result;
    }
    const QString decoded = QString::fromUtf8(nativeName);
    if (hasControlCharacter(decoded)) {
        result.problem = u"its name holds a control character"_s;
        return result;
    }
    result.key = QFile::decodeName(nativeName.toByteArray());
    QString virtualName = decoded.normalized(QString::NormalizationForm_C);
    virtualName.replace(u'\\', QChar(kBackslashReplacement));
    result.virtualName = std::move(virtualName);
    return result;
}

QString cleanAlias(QStringView alias)
{
    QString cleaned;
    const QString normalised = alias.toString().normalized(QString::NormalizationForm_C);
    cleaned.reserve(normalised.size());
    for (const QChar c : normalised) {
        if (c.category() == QChar::Other_Control) {
            continue;
        }
        cleaned.append(c == u'\\' || c == u'/' ? QChar(u'_') : c);
    }
    cleaned = cleaned.trimmed();
    if (cleaned.size() > kMaxAliasChars) {
        cleaned.truncate(kMaxAliasChars);
        if (cleaned.back().isHighSurrogate()) {
            cleaned.chop(1);
        }
        cleaned = cleaned.trimmed();
    }
    if (cleaned.isEmpty() || cleaned == u"."_s || cleaned == u".."_s) {
        return u"Shared"_s;
    }
    return cleaned;
}

QStringList uniqueAliases(const QStringList &aliases)
{
    QStringList unique;
    unique.reserve(aliases.size());
    QSet<QString> taken; // case-folded
    for (const QString &alias : aliases) {
        const QString base = cleanAlias(alias);
        QString candidate = base;
        for (int n = 2; taken.contains(candidate.toCaseFolded()); ++n) {
            candidate = u"%1 (%2)"_s.arg(base, QString::number(n));
        }
        taken.insert(candidate.toCaseFolded());
        unique.append(candidate);
    }
    return unique;
}

QString foldWord(QStringView word)
{
    if (isAscii(word)) {
        return word.toString();
    }
    QString stripped;
    const QList<uint> decomposed = word.toString().normalized(QString::NormalizationForm_KD).toUcs4();
    for (const uint c : decomposed) {
        if (QChar::category(static_cast<char32_t>(c)) != QChar::Mark_NonSpacing) {
            stripped.append(QStringView(QChar::fromUcs4(static_cast<char32_t>(c))));
        }
    }
    QString folded;
    const QList<uint> letters = stripped.toCaseFolded().toUcs4();
    for (const uint c : letters) {
        const QStringView latin = transliteration(static_cast<char32_t>(c));
        folded.append(latin.isEmpty() ? QStringView(QChar::fromUcs4(static_cast<char32_t>(c))) : latin);
    }
    return folded.normalized(QString::NormalizationForm_C);
}

QStringList searchWords(QStringView text)
{
    const QStringList words = match::QueryPlanner::responderWords(text);
    QStringList folded;
    folded.reserve(words.size());
    for (const QString &word : words) {
        QString f = foldWord(word);
        if (!f.isEmpty()) {
            folded.append(std::move(f));
        }
    }
    return folded;
}

qsizetype codePointCount(QStringView text)
{
    qsizetype count = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            ++i;
        }
        ++count;
    }
    return count;
}

} // namespace selecta::share::detail
