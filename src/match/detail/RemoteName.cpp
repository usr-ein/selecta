// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/RemoteName.h"

#include "match/Lexicon.h"
#include "match/Normalizer.h"
#include "match/VersionClassifier.h"
#include "match/detail/Text.h"

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::match::detail {

namespace {

QString normalizeDashesAndQuotes(QStringView text)
{
    QString out = text.toString();
    for (QChar &c : out) {
        switch (c.unicode()) {
        case 0x2010: // ‐
        case 0x2011: // ‑
        case 0x2012: // ‒
        case 0x2013: // –
        case 0x2014: // —
        case 0x2015: // ―
        case 0x2212: // −
            c = u'-';
            break;
        case 0x2018:
        case 0x2019:
        case 0x201B:
        case 0x02BC:
            c = u'\'';
            break;
        case 0x201C:
        case 0x201D:
            c = u'"';
            break;
        default:
            break;
        }
    }
    return out;
}

QString collapseSpaces(QStringView text)
{
    return text.toString().simplified();
}

bool isWebsite(QStringView word)
{
    const QStringView text = word.trimmed();
    if (text.contains(u' ') || !text.contains(u'.')) {
        return false;
    }
    if (text.startsWith(u"www.", Qt::CaseInsensitive)) {
        return true;
    }
    const qsizetype dot = text.lastIndexOf(u'.');
    const QStringView domain = text.first(dot);
    const QString tld = text.sliced(dot + 1).toString().toLower();
    return domain.size() >= 2 && lexicon::isWebsiteTld(tld) &&
           std::ranges::all_of(domain, [](QChar c) { return c.isLetterOrNumber() || c == u'-' || c == u'.'; });
}

bool isQualityTokens(const QStringList &tokens)
{
    if (tokens.isEmpty()) {
        return false;
    }
    // "16-44.1", "24 96": a bit depth followed by a sample rate.
    if (tokens.size() >= 2 && (tokens.at(0) == u"16" || tokens.at(0) == u"24" || tokens.at(0) == u"32")) {
        static constexpr std::array<QStringView, 7> kRates{u"44", u"48", u"88", u"96", u"176", u"192", u"441"};
        if (std::ranges::find(kRates, QStringView(tokens.at(1))) != kRates.end()) {
            return std::ranges::all_of(tokens, [](const QString &t) {
                return isDigits(t) || lexicon::qualityWord(t) != lexicon::QualityWord::None;
            });
        }
    }
    bool strong = false;
    for (const QString &token : tokens) {
        const lexicon::QualityWord word = lexicon::qualityWord(token);
        if (word == lexicon::QualityWord::None && !lexicon::isReleaseTypeWord(token)) {
            return false;
        }
        strong = strong || word == lexicon::QualityWord::Strong;
    }
    return strong;
}

// "DC123", "ABC 123", "LABEL042", "BRC-123A": letters, then digits.
bool isCatalogue(QStringView text)
{
    const QString compact = text.trimmed().toString().remove(u' ').remove(u'-');
    if (compact.size() < 3 || compact.size() > 14) {
        return false;
    }
    qsizetype i = 0;
    while (i < compact.size() && compact.at(i).isLetter() && compact.at(i).unicode() < 0x80) {
        ++i;
    }
    const qsizetype letters = i;
    while (i < compact.size() && compact.at(i).isDigit()) {
        ++i;
    }
    const qsizetype digits = i - letters;
    if (i < compact.size() && compact.at(i).isLetter() && i + 1 == compact.size()) {
        ++i; // one trailing letter: "123A"
    }
    return letters >= 2 && letters <= 8 && digits >= 1 && digits <= 5 && i == compact.size();
}

// What a bracket group of a name is.
enum class GroupRole : quint8 {
    Keep, // version, feat. or title text: stays in the name
    Year,
    Quality,
    Catalogue,
    Disc,
    ReleaseType, // "[LP]", "(EP)", "(Single)"
    Junk,        // web junk, a website
};

GroupRole groupRole(QStringView content, const QStringList &tokens)
{
    const QString normalised = tokens.join(u' ');
    if (normalised.isEmpty()) {
        return GroupRole::Junk;
    }
    if (tokens.size() == 1 && isYearToken(tokens.front())) {
        return GroupRole::Year;
    }
    if (isQualityTokens(tokens)) {
        return GroupRole::Quality;
    }
    if (std::ranges::all_of(tokens, [](const QString &t) { return lexicon::isReleaseTypeWord(t); })) {
        return GroupRole::ReleaseType;
    }
    if (discNumberOf(content)) {
        return GroupRole::Disc;
    }
    if (isWebsite(content) || lexicon::isWebJunkPhrase(normalised)) {
        return GroupRole::Junk;
    }
    if (!VersionClassifier::classify(content) && isCatalogue(content)) {
        return GroupRole::Catalogue;
    }
    return GroupRole::Keep;
}

QString stripWebsites(QStringView text, QList<ParseRule> &rules)
{
    QString out = text.toString();
    // "www.site.com - Artist - Title" and "Artist - Title - www.site.com"
    const QStringList parts = splitTopLevel(out, u" - ");
    if (parts.size() >= 2 && isWebsite(parts.front())) {
        addRule(rules, ParseRule::WebsiteStripped);
        out = parts.mid(1).join(u" - ");
    }
    const QStringList again = splitTopLevel(out, u" - ");
    if (again.size() >= 2 && isWebsite(again.back())) {
        addRule(rules, ParseRule::WebsiteStripped);
        out = again.first(again.size() - 1).join(u" - ");
    }
    return out;
}

void applyGroup(CleanName &name, GroupRole role, const QString &content, const QStringList &tokens)
{
    switch (role) {
    case GroupRole::Year:
        if (name.year == 0) {
            name.year = tokens.front().toInt();
        }
        addRule(name.rules, ParseRule::YearTag);
        break;
    case GroupRole::Quality:
        name.quality = name.quality.isEmpty() ? content : name.quality + u' ' + content;
        for (const QString &token : tokens) {
            if (const QStringView source = lexicon::sceneSource(token); !source.isEmpty() && name.source.isEmpty()) {
                name.source = source.toString();
            }
            if (token == u"vinyl" || token == u"lp") {
                name.vinylHint = true;
            }
        }
        addRule(name.rules, ParseRule::QualityTag);
        break;
    case GroupRole::Catalogue:
        if (name.catalogue.isEmpty()) {
            name.catalogue = content;
        }
        addRule(name.rules, ParseRule::CatalogueNumber);
        break;
    case GroupRole::Disc:
        name.disc = discNumberOf(content).value_or(0);
        break;
    case GroupRole::ReleaseType:
        name.vinylHint = name.vinylHint || tokens.contains(u"lp"_s);
        addRule(name.rules, ParseRule::QualityTag);
        break;
    case GroupRole::Junk:
        addRule(name.rules, isWebsite(content) ? ParseRule::WebsiteStripped : ParseRule::WebJunkStripped);
        break;
    case GroupRole::Keep:
        break;
    }
}

// "(Daft Punk - One More Time) Robin Schulz Remix" → "Daft Punk - One More Time (Robin Schulz Remix)".
QString unwrapLeadingGroup(const QString &text)
{
    if (text.isEmpty() || !isOpenBracket(text.front())) {
        return text;
    }
    const qsizetype close = matchingBracket(text, 0);
    if (close < 0) {
        return text;
    }
    const QStringView content = QStringView(text).sliced(1, close - 1).trimmed();
    if (splitTopLevel(content, u" - ").size() < 2) {
        return text;
    }
    const QString rest = QStringView(text).sliced(close + 1).trimmed().toString();
    if (rest.isEmpty()) {
        return content.toString();
    }
    if (VersionClassifier::classify(rest)) {
        return content.toString() + u" (" + rest + u')';
    }
    return content.toString() + u' ' + rest;
}

qsizetype readDigits(QStringView text, qsizetype from, qsizetype max)
{
    qsizetype i = from;
    while (i < text.size() && i - from < max && text[i].isDigit() && text[i].unicode() < 0x80) {
        ++i;
    }
    if (i < text.size() && text[i].isDigit()) {
        return -1; // too many digits: "1999"
    }
    return i - from;
}

// The separator after a number: "\s*[-.)_]\s*" or "\s+". Returns its length (0 when there is none) and whether it
// had punctuation.
std::pair<qsizetype, bool> separatorAt(QStringView text, qsizetype from)
{
    qsizetype i = from;
    while (i < text.size() && text[i].isSpace()) {
        ++i;
    }
    bool punctuation = false;
    if (i < text.size() && (text[i] == u'-' || text[i] == u'.' || text[i] == u')' || text[i] == u'_')) {
        punctuation = true;
        ++i;
        while (i < text.size() && text[i].isSpace()) {
            ++i;
        }
    }
    return {i - from, punctuation};
}

} // namespace

// Splits on top-level occurrences of `separator` (not inside brackets).
QStringList splitTopLevel(QStringView text, QStringView separator)
{
    QStringList parts;
    int depth = 0;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (isOpenBracket(text[i])) {
            ++depth;
        } else if (isCloseBracket(text[i]) && depth > 0) {
            --depth;
        } else if (depth == 0 && text[i] == separator.front() && text.sliced(i).startsWith(separator)) {
            parts.append(text.sliced(start, i - start).trimmed().toString());
            i += separator.size() - 1;
            start = i + 1;
        }
    }
    parts.append(text.sliced(start).trimmed().toString());
    parts.removeIf([](const QString &part) { return part.isEmpty(); });
    return parts;
}

std::optional<int> discNumberOf(QStringView text)
{
    // "CD2", "CD 2", "Disc 2", "Disk-2", "DVD1", "Disc Two"
    const QString lower = text.trimmed().toString().toLower();
    static constexpr std::array<QStringView, 4> kWords{u"disc", u"disk", u"cd", u"dvd"};
    for (const QStringView word : kWords) {
        if (!lower.startsWith(word)) {
            continue;
        }
        QStringView rest = QStringView(lower).sliced(word.size());
        while (!rest.isEmpty() && (rest.front().isSpace() || rest.front() == u'-' || rest.front() == u'_' ||
                                   rest.front() == u'.' || rest.front() == u'#')) {
            rest.slice(1);
        }
        if (isDigits(rest) && rest.size() <= 2) {
            return rest.toInt();
        }
        if (const auto number = lexicon::numberWord(rest)) {
            return number;
        }
    }
    return std::nullopt;
}

void addRule(QList<ParseRule> &rules, ParseRule rule)
{
    if (!rules.contains(rule)) {
        rules.append(rule);
    }
}

CleanName cleanName(QStringView raw)
{
    CleanName name;
    QString text = Normalizer::repairMojibake(raw);
    if (text != raw) {
        addRule(name.rules, ParseRule::MojibakeRepaired);
    }
    text = normalizeDashesAndQuotes(text.normalized(QString::NormalizationForm_C));
    const bool hasSpace = text.contains(u' ');
    if (!hasSpace || text.count(u'_') >= 2) {
        text.replace(u'_', u' ');
    }
    if (!text.contains(u' ') && text.count(u'.') >= 2) {
        text.replace(u'.', u' '); // "Artist.-.Title.Extended.Mix"
    }
    text = stripWebsites(collapseSpaces(text), name.rules);
    text = unwrapLeadingGroup(text);

    // Bracket groups: extract tags, drop junk, keep the rest.
    QString kept;
    QString plain;
    qsizetype i = 0;
    while (i < text.size()) {
        if (!isOpenBracket(text.at(i))) {
            kept.append(text.at(i));
            plain.append(text.at(i));
            ++i;
            continue;
        }
        const qsizetype close = matchingBracket(text, i);
        if (close < 0) {
            kept.append(text.sliced(i)); // unbalanced: a truncated name, left to the title decomposition
            plain.append(text.sliced(i));
            break;
        }
        const QString content = text.sliced(i + 1, close - i - 1).trimmed();
        const QStringList tokens = Normalizer::tokens(content);
        const GroupRole role = groupRole(content, tokens);
        const bool leading = QStringView(kept).trimmed().isEmpty();
        if (role == GroupRole::Keep) {
            name.brackets.append(content);
            if (!leading) {
                kept.append(text.sliced(i, close - i + 1)); // a leading group ("[Label] …") leaves the name
            }
            plain.append(u' ');
        } else {
            applyGroup(name, role, content, tokens);
            kept.append(u' ');
            plain.append(u' ');
        }
        i = close + 1;
    }
    name.text = collapseSpaces(kept);
    name.plain = collapseSpaces(plain);
    // Separators left dangling by removed groups: "Artist - Title - " → "Artist - Title".
    while (name.text.endsWith(u" -") || name.text.endsWith(u'-')) {
        name.text = name.text.first(name.text.size() - 1).trimmed();
    }
    while (name.plain.endsWith(u" -") || name.plain.endsWith(u'-')) {
        name.plain = name.plain.first(name.plain.size() - 1).trimmed();
    }
    return name;
}

std::optional<Prefix> scanPrefix(QStringView text)
{
    Prefix prefix;
    if (text.isEmpty()) {
        return std::nullopt;
    }
    // Vinyl side and position: "A1 Title", "B2. Title".
    if (text.front() >= u'A' && text.front() <= u'H') {
        const qsizetype digits = readDigits(text, 1, 2);
        if (digits > 0) {
            const auto [separator, punctuation] = separatorAt(text, 1 + digits);
            if (separator > 0 || 1 + digits == text.size()) {
                prefix.vinyl = text.first(1 + digits).toString();
                prefix.length = 1 + digits + separator;
                prefix.punctuation = punctuation;
                prefix.rest = prefix.length < text.size();
                return prefix;
            }
        }
        return std::nullopt;
    }
    const qsizetype first = readDigits(text, 0, 3);
    if (first <= 0) {
        return std::nullopt;
    }
    // Disc and track: "1-05 Title", "2.03 Title".
    if (first <= 2 && first < text.size() && (text[first] == u'-' || text[first] == u'.')) {
        const qsizetype second = readDigits(text, first + 1, 3);
        if (second > 0) {
            const auto [separator, punctuation] = separatorAt(text, first + 1 + second);
            const qsizetype end = first + 1 + second;
            if (separator > 0 && end + separator < text.size()) {
                prefix.disc = text.first(first).toInt();
                prefix.track = text.sliced(first + 1, second).toInt();
                prefix.digits = static_cast<int>(second);
                prefix.length = end + separator;
                prefix.punctuation = true;
                prefix.leadingZero = text[first + 1] == u'0';
                prefix.rest = true;
                return prefix;
            }
        }
    }
    const auto [separator, punctuation] = separatorAt(text, first);
    if (separator == 0 && first != text.size()) {
        return std::nullopt; // "10cc", "2Pac": the number is part of a word
    }
    prefix.track = text.first(first).toInt();
    prefix.digits = static_cast<int>(first);
    prefix.length = first + separator;
    prefix.punctuation = punctuation;
    prefix.leadingZero = first >= 2 && text.front() == u'0';
    prefix.rest = prefix.length < text.size();
    return prefix;
}

// "Track 01", "AudioTrack 01", "Track01", "Piste 3".
std::optional<int> ripNumber(QStringView text)
{
    const QStringList tokens = Normalizer::tokens(text);
    if (tokens.size() == 2 && lexicon::isRipWord(tokens.front()) && isDigits(tokens.back())) {
        return tokens.back().toInt();
    }
    if (tokens.size() == 3 && tokens.front() == u"audio" && lexicon::isRipWord(tokens.at(1)) &&
        isDigits(tokens.back())) {
        return tokens.back().toInt();
    }
    if (tokens.size() == 1) {
        const QString &token = tokens.front();
        qsizetype letters = 0;
        while (letters < token.size() && token.at(letters).isLetter()) {
            ++letters;
        }
        if (letters > 0 && letters < token.size() && lexicon::isRipWord(token.first(letters)) &&
            isDigits(QStringView(token).sliced(letters))) {
            return QStringView(token).sliced(letters).toInt();
        }
    }
    return std::nullopt;
}

} // namespace selecta::match::detail
