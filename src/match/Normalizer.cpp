// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Normalizer.h"

#include "match/Lexicon.h"
#include "match/VersionClassifier.h"
#include "match/detail/Text.h"

#include <QByteArray>
#include <QChar>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace lexicon = detail::lexicon;

namespace {

constexpr char16_t kKeySeparator = u'␟'; // ␟, never produced by normalisation

QStringView bounded(QStringView text)
{
    return text.size() > Normalizer::kMaxFieldLength ? text.first(Normalizer::kMaxFieldLength) : text;
}

// --- Mojibake ----------------------------------------------------------------------------------------------------

// The Windows-1252 byte (0x80–0x9F) a character stands for, or 0.
char cp1252Byte(char16_t c)
{
    static constexpr std::array<std::pair<char16_t, unsigned char>, 27> kTable{{
        {0x0152, 0x8C}, {0x0153, 0x9C}, {0x0160, 0x8A}, {0x0161, 0x9A}, {0x0178, 0x9F}, {0x017D, 0x8E}, {0x017E, 0x9E},
        {0x0192, 0x83}, {0x02C6, 0x88}, {0x02DC, 0x98}, {0x2013, 0x96}, {0x2014, 0x97}, {0x2018, 0x91}, {0x2019, 0x92},
        {0x201A, 0x82}, {0x201C, 0x93}, {0x201D, 0x94}, {0x201E, 0x84}, {0x2020, 0x86}, {0x2021, 0x87}, {0x2022, 0x95},
        {0x2026, 0x85}, {0x2030, 0x89}, {0x2039, 0x8B}, {0x203A, 0x9B}, {0x20AC, 0x80}, {0x2122, 0x99},
    }};
    const auto *it = std::ranges::lower_bound(kTable, c, {}, &std::pair<char16_t, unsigned char>::first);
    if (it == kTable.end() || it->first != c) {
        return 0;
    }
    return static_cast<char>(it->second);
}

// A UTF-8 lead byte read as Latin-1 ("Ã", "Â"…: U+00C2–U+00F4) followed by a continuation byte read the same way.
bool looksLikeMojibake(QStringView text)
{
    for (qsizetype i = 0; i + 1 < text.size(); ++i) {
        const char16_t lead = text[i].unicode();
        if (lead < 0xC2 || lead > 0xF4) {
            continue;
        }
        const char16_t next = text[i + 1].unicode();
        if ((next >= 0x80 && next <= 0xBF) || cp1252Byte(next) != 0) {
            return true;
        }
    }
    return false;
}

std::optional<QString> repairOnce(QStringView text)
{
    if (!looksLikeMojibake(text)) {
        return std::nullopt;
    }
    QByteArray bytes;
    bytes.reserve(text.size());
    for (const QChar c : text) {
        const char16_t unit = c.unicode();
        if (unit <= 0xFF) {
            bytes.append(static_cast<char>(unit));
        } else if (const char byte = cp1252Byte(unit); byte != 0) {
            bytes.append(byte);
        } else {
            return std::nullopt; // a character Latin-1 can't hold: this isn't mojibake
        }
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString decoded = decoder.decode(bytes);
    if (decoder.hasError() || decoded == text) {
        return std::nullopt;
    }
    const bool nonAscii = std::ranges::any_of(decoded, [](QChar c) { return c.unicode() >= 0x80; });
    if (!nonAscii) {
        return std::nullopt;
    }
    return decoded;
}

// --- Raw-text rules ----------------------------------------------------------------------------------------------

// "Prodigy, The" → "The Prodigy"; also ", A" and ", An" (beets). Artist fields only.
QString moveArticleSuffix(QStringView text)
{
    const QStringView trimmed = text.trimmed();
    const qsizetype comma = trimmed.lastIndexOf(u',');
    if (comma <= 0) {
        return trimmed.toString();
    }
    const QStringView article = trimmed.sliced(comma + 1).trimmed();
    if (article.compare(u"the", Qt::CaseInsensitive) == 0 || article.compare(u"a", Qt::CaseInsensitive) == 0 ||
        article.compare(u"an", Qt::CaseInsensitive) == 0) {
        return article.toString() + u' ' + trimmed.first(comma).trimmed();
    }
    return trimmed.toString();
}

bool isSingleLetterAt(QStringView text, qsizetype i)
{
    if (i < 0 || i >= text.size() || !text[i].isLetter()) {
        return false;
    }
    const bool startsWord = i == 0 || !text[i - 1].isLetterOrNumber();
    const bool endsWord = i + 1 == text.size() || !text[i + 1].isLetterOrNumber();
    return startsWord && endsWord;
}

// Dots between single letters are removed: "A.T.F.C." → "ATFC", "t.A.T.u." → "tATu"; "J. Cole", "Dr. Dre" untouched.
QString removeAcronymDots(QStringView text)
{
    if (!text.contains(u'.')) {
        return text.toString();
    }
    QString out;
    out.reserve(text.size());
    qsizetype i = 0;
    while (i < text.size()) {
        if (isSingleLetterAt(text, i) && i + 1 < text.size() && text[i + 1] == u'.') {
            // Collect "L." pairs; a final letter may lack its dot.
            QString letters;
            qsizetype pos = i;
            while (isSingleLetterAt(text, pos)) {
                letters.append(text[pos]);
                if (pos + 1 < text.size() && text[pos + 1] == u'.') {
                    pos += 2;
                } else {
                    pos += 1;
                    break;
                }
            }
            if (letters.size() >= 2) {
                out.append(letters);
                i = pos;
                continue;
            }
        }
        out.append(text[i]);
        ++i;
    }
    return out;
}

QString prepare(QStringView text, FieldKind kind)
{
    QString prepared = Normalizer::repairMojibake(bounded(text));
    if (kind == FieldKind::Artist) {
        prepared = moveArticleSuffix(prepared);
    }
    return Normalizer::splitCamelCase(removeAcronymDots(prepared));
}

// --- Folding -----------------------------------------------------------------------------------------------------

bool isWordCategory(char32_t codePoint)
{
    if (QChar::isLetterOrNumber(codePoint)) {
        return true;
    }
    const QChar::Category category = QChar::category(codePoint);
    return category == QChar::Mark_SpacingCombining || category == QChar::Mark_Enclosing;
}

bool isLeetSymbol(char32_t codePoint)
{
    return codePoint == U'$' || codePoint == U'@' || codePoint == U'!';
}

bool isAscii(QStringView text)
{
    return std::ranges::all_of(text, [](QChar c) { return c.unicode() < 0x80; });
}

bool isAsciiWordCharacter(char16_t c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9');
}

// foldTokens() for ASCII text, which NFKD, NFC, mark removal and transliteration leave alone: the same tokens without
// the Unicode work (most file names are ASCII). A property test holds it to the general path.
QStringList foldAsciiTokens(QStringView prepared, bool keepLeetSymbols)
{
    QStringList tokens;
    QString word;
    const auto flush = [&tokens, &word] {
        if (!word.isEmpty()) {
            tokens.append(std::exchange(word, QString()));
        }
    };
    const qsizetype n = prepared.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t c = prepared[i].unicode();
        if (isAsciiWordCharacter(c)) {
            word.append(QChar(c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c - u'A' + u'a') : c));
            continue;
        }
        const bool leetSymbolInWord = keepLeetSymbols && isLeetSymbol(c) && !word.isEmpty() && i + 1 < n &&
                                      isAsciiWordCharacter(prepared[i + 1].unicode());
        if (leetSymbolInWord) {
            word.append(QChar(c));
        } else if (c == u'&' || c == u'+') {
            flush();
            tokens.append(u"and"_s);
        } else if (c == u'$' && !keepLeetSymbols) {
            word.append(u's');
        } else if (lexicon::isApostrophe(c)) {
            // removed without a space: "What's" → "whats"
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

// Steps 1–6 (note §4.2): NFKD, case fold, non-spacing marks dropped, transliteration, symbol rules, tokens. With
// `keepLeetSymbols`, "$", "@" and "!" between word characters stay in the word (for the leet key).
QStringList foldTokens(QStringView prepared, bool keepLeetSymbols)
{
    if (isAscii(prepared)) {
        return foldAsciiTokens(prepared, keepLeetSymbols);
    }
    const QString folded = prepared.toString().normalized(QString::NormalizationForm_KD).toCaseFolded();
    QStringList tokens;
    QString word;
    const auto flush = [&tokens, &word] {
        if (!word.isEmpty()) {
            tokens.append(word.normalized(QString::NormalizationForm_C));
            word.clear();
        }
    };
    const qsizetype n = folded.size();
    for (qsizetype i = 0; i < n;) {
        const auto [codePoint, width] = detail::codePointAt(folded, i);
        const QStringView unit = QStringView(folded).sliced(i, width);
        i += width;
        if (QChar::category(codePoint) == QChar::Mark_NonSpacing) {
            continue;
        }
        const bool leetSymbolInWord = keepLeetSymbols && isLeetSymbol(codePoint) && !word.isEmpty() && i < n &&
                                      isWordCategory(detail::codePointAt(folded, i).first);
        if (const QStringView latin = lexicon::transliteration(codePoint); !latin.isEmpty()) {
            word.append(latin);
        } else if (isWordCategory(codePoint) || leetSymbolInWord) {
            word.append(unit);
        } else if (codePoint == U'&' || codePoint == U'+') {
            flush();
            tokens.append(u"and"_s);
        } else if (codePoint == U'$' && !keepLeetSymbols) {
            word.append(u's');
        } else if (lexicon::isApostrophe(codePoint)) {
            // removed without a space: "What's" → "whats"
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

std::optional<int> romanValue(QStringView token)
{
    static constexpr int kMaxRoman = 39; // i … xxxix: enough for parts and volumes, and never "mix" (1009)
    if (token.isEmpty() || token.size() > 6) {
        return std::nullopt;
    }
    const auto digit = [](QChar c) -> int {
        switch (c.unicode()) {
        case u'i':
            return 1;
        case u'v':
            return 5;
        case u'x':
            return 10;
        default:
            return 0;
        }
    };
    int value = 0;
    for (qsizetype i = 0; i < token.size(); ++i) {
        const int current = digit(token[i]);
        if (current == 0) {
            return std::nullopt;
        }
        const int next = i + 1 < token.size() ? digit(token[i + 1]) : 0;
        value += current < next ? -current : current;
    }
    if (value < 1 || value > kMaxRoman) {
        return std::nullopt;
    }
    // Only canonical numerals: "iiii" and "vx" are words, not numbers.
    static constexpr std::array<QStringView, 10> kOnes{u"",  u"i",  u"ii",  u"iii",  u"iv",
                                                       u"v", u"vi", u"vii", u"viii", u"ix"};
    static constexpr std::array<QStringView, 4> kTens{u"", u"x", u"xx", u"xxx"};
    const QString canonical =
        kTens.at(static_cast<std::size_t>(value / 10)).toString() + kOnes.at(static_cast<std::size_t>(value % 10));
    if (canonical != token) {
        return std::nullopt;
    }
    return value;
}

// Steps 7 and 8 (note §4.2).
void applyFieldRules(QStringList &tokens, FieldKind kind)
{
    if (kind == FieldKind::Artist) {
        if (tokens.size() >= 2 && tokens.front() == u"the" && tokens.at(1) != u"the") {
            tokens.removeFirst();
        }
        return;
    }
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        if (const QStringView canonical = lexicon::canonicalToken(tokens.at(i)); !canonical.isEmpty()) {
            tokens[i] = canonical.toString();
        }
        if (i > 0 && lexicon::isNumberingWord(tokens.at(i - 1))) {
            if (const auto roman = romanValue(tokens.at(i))) {
                tokens[i] = QString::number(*roman);
            } else if (const auto word = lexicon::numberWord(tokens.at(i))) {
                tokens[i] = QString::number(*word);
            }
        }
    }
}

bool hasLetter(QStringView token)
{
    return std::ranges::any_of(token, [](QChar c) { return c.isLetter(); });
}

bool hasLeetCharacter(QStringView token)
{
    return std::ranges::any_of(token, [](QChar c) { return c.isDigit() || isLeetSymbol(c.unicode()); });
}

QString leetFoldToken(const QString &token)
{
    if (!hasLetter(token) || !hasLeetCharacter(token)) {
        return token;
    }
    QString folded = token;
    for (QChar &c : folded) {
        if (const char16_t replacement = lexicon::leetFold(c.unicode()); replacement != 0) {
            c = QChar(replacement);
        }
    }
    return folded;
}

bool mayHaveLeet(QStringView text)
{
    return std::ranges::any_of(text, [](QChar c) { return c.isDigit() || isLeetSymbol(c.unicode()); });
}

// " & ", ", ", " and ", " x ", " × ", " vs ", " vs. ", " + ", "; ", " / ", " with ": matching sets only (note §4.1 step
// 7).
QStringList splitArtists(QStringView text)
{
    static constexpr std::array<QStringView, 12> kSeparators{u" & ",  u", ",  u" and ", u" x ", u" × ",    u" vs. ",
                                                             u" vs ", u" + ", u"; ",    u" / ", u" with ", u" ft "};
    QStringList parts;
    qsizetype start = 0;
    qsizetype i = 0;
    while (i < text.size()) {
        qsizetype matched = 0;
        for (const QStringView separator : kSeparators) {
            if (text.sliced(i).startsWith(separator, Qt::CaseInsensitive)) {
                matched = separator.size();
                break;
            }
        }
        if (matched > 0) {
            parts.append(text.sliced(start, i - start).toString());
            i += matched;
            start = i;
        } else {
            ++i;
        }
    }
    parts.append(text.sliced(start).toString());
    return parts;
}

QStringList artistSet(const QStringList &names)
{
    QStringList keys;
    for (const QString &name : names) {
        const QStringList members = splitArtists(name);
        for (const QString &member : members) {
            QString key = Normalizer::normalize(member, FieldKind::Artist);
            if (!key.isEmpty()) {
                keys.append(std::move(key));
            }
        }
    }
    keys.sort();
    keys.removeDuplicates();
    return keys;
}

// Featured artists of an artist field: "(feat. X)", "[ft. X]", " feat. X", " ft. X", " featuring X".
struct FeatSplit
{
    QString primary;
    QStringList featured;
};

FeatSplit splitFeatured(QStringView artist)
{
    FeatSplit split;
    QString text = artist.toString();
    // Bracketed groups first.
    for (qsizetype open = detail::indexOfAny(text, u"([{", 0); open >= 0;
         open = detail::indexOfAny(text, u"([{", open + 1)) {
        const qsizetype close = detail::matchingBracket(text, open);
        if (close < 0) {
            break;
        }
        const QStringView content = QStringView(text).sliced(open + 1, close - open - 1).trimmed();
        if (const qsizetype skip = detail::featMarkerLength(content, /*inBrackets=*/true); skip > 0) {
            split.featured.append(content.sliced(skip).trimmed().toString());
            text.remove(open, close - open + 1);
            open -= 1;
        }
    }
    // Then an unbracketed marker: the rest of the field is featured.
    if (const qsizetype marker = detail::findFeatMarker(text); marker >= 0) {
        const qsizetype skip = detail::featMarkerLength(QStringView(text).sliced(marker + 1), /*inBrackets=*/false);
        split.featured.append(QStringView(text).sliced(marker + 1 + skip).trimmed().toString());
        text.truncate(marker);
    }
    split.primary = text.trimmed();
    return split;
}

// Album edition text: "(Deluxe Edition)", "[Remastered 2011]", "(2019)", "[FLAC]", " - Expanded Edition".
bool isEditionGroup(QStringView content)
{
    const QStringList tokens = Normalizer::tokens(content);
    if (tokens.isEmpty()) {
        return true;
    }
    bool strong = false;
    for (const QString &token : tokens) {
        const bool number = std::ranges::all_of(token, [](QChar c) { return c.isDigit(); });
        if (lexicon::isStrongEditionWord(token)) {
            strong = true;
        } else if (!number && !lexicon::isEditionWord(token) &&
                   lexicon::qualityWord(token) == lexicon::QualityWord::None) {
            return false;
        }
    }
    if (strong) {
        return true;
    }
    // Without an edition word: a year, a quality tag or a release type on its own.
    return detail::isYearToken(tokens.join(QString())) || std::ranges::all_of(tokens, [](const QString &t) {
               return lexicon::qualityWord(t) == lexicon::QualityWord::Strong || lexicon::isReleaseTypeWord(t);
           });
}

QString stripAlbumEdition(QStringView album)
{
    QString text = album.trimmed().toString();
    for (bool changed = true; changed && !text.isEmpty();) {
        changed = false;
        const QStringView view(text);
        if (view.endsWith(u')') || view.endsWith(u']') || view.endsWith(u'}')) {
            const qsizetype open = detail::matchingOpenBracket(view, view.size() - 1);
            if (open > 0 && isEditionGroup(view.sliced(open + 1, view.size() - open - 2))) {
                text = view.first(open).trimmed().toString();
                changed = true;
                continue;
            }
        }
        const qsizetype dash = view.lastIndexOf(u" - ");
        if (dash > 0 && isEditionGroup(view.sliced(dash + 3))) {
            text = view.first(dash).trimmed().toString();
            changed = true;
        }
    }
    return text;
}

} // namespace

QString Normalizer::repairMojibake(QStringView text)
{
    QString current = text.toString();
    for (int pass = 0; pass < 2; ++pass) { // double encoding is the worst seen in practice
        std::optional<QString> repaired = repairOnce(current);
        if (!repaired) {
            break;
        }
        current = std::move(*repaired);
    }
    return current;
}

QString Normalizer::splitCamelCase(QStringView text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (i > 0 && c.isUpper()) {
            const QChar previous = text[i - 1];
            const bool nextLower = i + 1 < text.size() && text[i + 1].isLower();
            const bool secondLower = i + 2 < text.size() && text[i + 2].isLower();
            if ((previous.isLower() && nextLower) || (previous.isUpper() && nextLower && secondLower)) {
                out.append(u' ');
            }
        }
        out.append(c);
    }
    return out;
}

QStringList Normalizer::tokens(QStringView text, FieldKind kind)
{
    QStringList result = foldTokens(prepare(text, kind), /*keepLeetSymbols=*/false);
    applyFieldRules(result, kind);
    return result;
}

QString Normalizer::normalize(QStringView text, FieldKind kind)
{
    return tokens(text, kind).join(u' ');
}

QString Normalizer::squash(QStringView key)
{
    QString out;
    out.reserve(key.size());
    for (const QChar c : key) {
        if (c != u' ') {
            out.append(c);
        }
    }
    return out;
}

QString Normalizer::leetKey(QStringView text, FieldKind kind)
{
    QStringList words = foldTokens(prepare(text, kind), /*keepLeetSymbols=*/true);
    applyFieldRules(words, kind);
    QString key;
    for (const QString &word : std::as_const(words)) {
        key.append(leetFoldToken(word));
    }
    // Symbols that weren't between word characters must not survive (they never reach the primary key either).
    key.removeIf([](QChar c) { return isLeetSymbol(c.unicode()); });
    return key;
}

FieldKey Normalizer::fieldKey(QStringView text, FieldKind kind)
{
    FieldKey field;
    field.key = normalize(text, kind);
    field.squashed = squash(field.key);
    field.leet = mayHaveLeet(text) ? leetKey(text, kind) : field.squashed;
    return field;
}

ArtistKeys Normalizer::artistKeys(QStringView artist)
{
    const QString repaired = repairMojibake(bounded(artist));
    const FeatSplit split = splitFeatured(repaired);
    const QString primary = moveArticleSuffix(split.primary);
    ArtistKeys keys;
    keys.field = fieldKey(primary, FieldKind::Artist);
    keys.set = artistSet({primary});
    keys.featured = artistSet(split.featured);
    return keys;
}

TitleKeys Normalizer::titleKeys(QStringView title)
{
    TitleParts parts = VersionClassifier::decompose(title);
    TitleKeys keys;
    keys.field = fieldKey(parts.core, FieldKind::Title);
    QStringList extras;
    for (const QString &extra : std::as_const(parts.extras)) {
        QString key = normalize(extra, FieldKind::Title);
        if (!key.isEmpty()) {
            extras.append(std::move(key));
        }
    }
    extras.sort();
    extras.removeDuplicates();
    keys.extraKey = extras.join(u" / ");
    keys.version = std::move(parts.version);
    keys.featured = artistSet(parts.featured);
    keys.labelHint = std::move(parts.labelHint);
    return keys;
}

FieldKey Normalizer::albumKey(QStringView album)
{
    const QString repaired = repairMojibake(bounded(album));
    QString stripped = stripAlbumEdition(repaired);
    if (normalize(stripped, FieldKind::Album).isEmpty()) {
        stripped = repaired; // the whole title was edition text: keep it rather than lose the album
    }
    return fieldKey(stripped, FieldKind::Album);
}

TrackKeys Normalizer::trackKeys(QStringView artist, QStringView title, QStringView album)
{
    TrackKeys keys;
    keys.artist = artistKeys(artist);
    keys.title = titleKeys(title);
    keys.album = albumKey(album);
    const QString versionKey = keys.title.version.key();
    keys.exactKey = keys.artist.field.squashed + kKeySeparator + keys.title.field.squashed + kKeySeparator + versionKey;
    const QString firstArtist =
        keys.artist.set.isEmpty() ? keys.artist.field.squashed : squash(keys.artist.set.constFirst());
    keys.looseKey = firstArtist + kKeySeparator + keys.title.field.squashed;
    return keys;
}

} // namespace selecta::match
