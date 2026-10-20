// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Tracklist.h"

#include "match/Lexicon.h"
#include "match/VersionClassifier.h"
#include "match/detail/Text.h"

#include <QHash>

#include <array>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace {

QString unifyDashes(QStringView text)
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
        case 0x00A0: // no-break space
        case 0x2007:
        case 0x202F:
            c = u' ';
            break;
        default:
            break;
        }
    }
    return out.replace(u" -- "_s, u" - "_s);
}

bool isBullet(QChar c)
{
    switch (c.unicode()) {
    case u'-':
    case u'*':
    case u'+':
    case u'>':
    case 0x2022: // •
    case 0x00B7: // ·
    case 0x2023: // ‣
    case 0x2043: // ⁃
    case 0x25BA: // ►
    case 0x25B6: // ▶
    case 0x25CB: // ○
    case 0x25CF: // ●
    case 0x25A0: // ■
    case 0x25A1: // □
    case 0x25E6: // ◦
        return true;
    default:
        return false;
    }
}

bool stripBullet(QStringView &text)
{
    if (text.size() >= 2 && isBullet(text.front()) && text[1].isSpace()) {
        text = text.sliced(1).trimmed();
        return true;
    }
    return false;
}

qsizetype digitRun(QStringView text, qsizetype from, qsizetype max)
{
    qsizetype i = from;
    while (i < text.size() && i - from <= max && text[i] >= u'0' && text[i] <= u'9') {
        ++i;
    }
    return i - from;
}

qsizetype findSeparator(QStringView text);

bool isNumberingPunctuation(QChar c)
{
    return c == u'.' || c == u')' || c == u':' || c == u']';
}

// "01.", "1)", "1:", "#1", "#01 ", "01 " (a leading zero), "[01]", "(1)", and with `bareNumbers` also "1 ".
bool stripNumbering(QStringView &text, bool bareNumbers)
{
    qsizetype i = 0;
    const bool hash = text.startsWith(u'#');
    const bool bracket = text.startsWith(u'[') || text.startsWith(u'(');
    if (hash || bracket) {
        i = 1;
    }
    const qsizetype digits = digitRun(text, i, 3);
    if (digits == 0 || digits > 3 || i + digits >= text.size()) {
        return false;
    }
    const qsizetype end = i + digits;
    const QChar next = text[end];
    bool strip = false;
    qsizetype after = end;
    if (bracket) {
        strip = next == u']' || next == u')';
        after = end + 1;
    } else if (isNumberingPunctuation(next) || (next == u'-' && end + 1 < text.size() && text[end + 1] == u' ')) {
        strip = true;
        after = end + 1;
    } else if (next.isSpace()) {
        // "1 - Artist - Title" and "1 | Artist - Title": a number set off by a separator.
        const QStringView ahead = text.sliced(end).trimmed();
        const bool separated =
            (ahead.startsWith(u"- ") || ahead.startsWith(u"| ")) && findSeparator(ahead.sliced(2)) > 0;
        strip = hash || separated || (digits >= 2 && text.front() == u'0') || bareNumbers;
    }
    if (!strip) {
        return false;
    }
    QStringView rest = text.sliced(after).trimmed();
    if (rest.startsWith(u'-') || rest.startsWith(u'|')) {
        rest = rest.sliced(1).trimmed();
    }
    if (rest.isEmpty()) {
        return false; // a line that is only a number
    }
    text = rest;
    return true;
}

// "Track 1:", "Track 01 -", "No. 3".
bool stripTrackWord(QStringView &text)
{
    for (const QStringView word : {QStringView(u"track"), QStringView(u"no."), QStringView(u"no")}) {
        if (!text.startsWith(word, Qt::CaseInsensitive) || text.size() <= word.size() + 1 ||
            !text[word.size()].isSpace()) {
            continue;
        }
        QStringView rest = text.sliced(word.size()).trimmed();
        const qsizetype digits = digitRun(rest, 0, 3);
        if (digits == 0 || digits > 3) {
            return false;
        }
        rest = rest.sliced(digits).trimmed();
        if (!rest.isEmpty() && (isNumberingPunctuation(rest.front()) || rest.front() == u'-')) {
            rest = rest.sliced(1).trimmed();
        }
        if (rest.isEmpty()) {
            return false;
        }
        text = rest;
        return true;
    }
    return false;
}

// Discogs vinyl positions: "A1 Artist - Title", "B2. Artist - Title" (but not "A1 - Title": A1 is then the artist).
bool stripVinylPosition(QStringView &text)
{
    if (text.size() < 4 || text.front() < u'A' || text.front() > u'H') {
        return false;
    }
    const qsizetype digits = digitRun(text, 1, 2);
    if (digits == 0 || digits > 2) {
        return false;
    }
    qsizetype after = 1 + digits;
    if (after < text.size() && text[after] == u'.') {
        ++after;
    } else if (after >= text.size() || !text[after].isSpace()) {
        return false;
    }
    const QStringView rest = text.sliced(after).trimmed();
    if (rest.isEmpty() || rest.startsWith(u'-')) {
        return false;
    }
    text = rest;
    return true;
}

// "12:34", "1:02:03" → seconds.
std::optional<int> clockSeconds(QStringView text)
{
    const QList<QStringView> parts = text.split(u':');
    if (parts.size() < 2 || parts.size() > 3) {
        return std::nullopt;
    }
    int seconds = 0;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QStringView part = parts.at(i);
        const bool first = i == 0;
        if (!detail::isDigits(part) || (first ? part.size() > 2 : part.size() != 2)) {
            return std::nullopt;
        }
        const int value = part.toInt();
        if (!first && value >= 60) {
            return std::nullopt;
        }
        seconds = (seconds * 60) + value;
    }
    return seconds;
}

// A timestamp at the start: "12:34", "[12:34]", "(1:02:03)", followed by an optional "-" or "|".
bool stripTimestamp(QStringView &text, int &seconds)
{
    QStringView rest = text;
    QChar close;
    if (rest.startsWith(u'[') || rest.startsWith(u'(')) {
        close = rest.front() == u'[' ? u']' : u')';
        rest = rest.sliced(1);
    }
    qsizetype end = 0;
    while (end < rest.size() && (rest[end].isDigit() || rest[end] == u':')) {
        ++end;
    }
    const auto value = clockSeconds(rest.first(end));
    if (!value) {
        return false;
    }
    rest = rest.sliced(end);
    if (!close.isNull()) {
        if (!rest.startsWith(close)) {
            return false;
        }
        rest = rest.sliced(1);
    } else if (!rest.isEmpty() && !rest.front().isSpace()) {
        return false;
    }
    rest = rest.trimmed();
    if (rest.startsWith(u'-') || rest.startsWith(u'|')) {
        rest = rest.sliced(1).trimmed();
    }
    if (rest.isEmpty()) {
        return false;
    }
    seconds = *value;
    text = rest;
    return true;
}

// A length at the end: "Artist - Title 5:23", "Artist - Title (5:23)".
bool stripTrailingLength(QStringView &text, int &seconds)
{
    QStringView rest = text;
    bool bracketed = false;
    if (rest.endsWith(u')') || rest.endsWith(u']')) {
        rest.chop(1);
        bracketed = true;
    }
    qsizetype start = rest.size();
    while (start > 0 && (rest[start - 1].isDigit() || rest[start - 1] == u':')) {
        --start;
    }
    const auto value = clockSeconds(rest.sliced(start));
    if (!value || start == 0) {
        return false;
    }
    rest = rest.first(start);
    if (bracketed) {
        if (!rest.endsWith(u'(') && !rest.endsWith(u'[')) {
            return false;
        }
        rest.chop(1);
    }
    if (!rest.isEmpty() && !rest.back().isSpace()) {
        return false;
    }
    rest = rest.trimmed();
    if (rest.isEmpty()) {
        return false;
    }
    seconds = *value;
    text = rest;
    return true;
}

// A trailing "[LABEL]": not a version, feat., remaster or junk group.
bool stripLabel(QStringView &text, QString &label)
{
    if (!text.endsWith(u']')) {
        return false;
    }
    const qsizetype open = detail::matchingOpenBracket(text, text.size() - 1);
    if (open <= 0 || text[open] != u'[') {
        return false;
    }
    const QStringView content = text.sliced(open + 1, text.size() - open - 2).trimmed();
    const QString normalised = Normalizer::normalize(content);
    if (normalised.isEmpty() || VersionClassifier::classify(content) ||
        detail::featMarkerLength(content, /*inBrackets=*/true) > 0 || detail::lexicon::isWebJunkPhrase(normalised)) {
        return false;
    }
    label = content.toString();
    text = text.first(open).trimmed();
    return true;
}

bool isLink(QStringView text)
{
    return text.startsWith(u"http://", Qt::CaseInsensitive) || text.startsWith(u"https://", Qt::CaseInsensitive) ||
           text.startsWith(u"spotify:", Qt::CaseInsensitive) || text.startsWith(u"www.", Qt::CaseInsensitive);
}

bool isHeading(QStringView text)
{
    const QString key = Normalizer::normalize(text);
    static constexpr std::array<QStringView, 6> kHeadings{u"tracklist", u"track list", u"setlist",
                                                          u"set list",  u"playlist",   u"tracks"};
    return key.isEmpty() || std::ranges::find(kHeadings, QStringView(key)) != kHeadings.end();
}

// Only a number with its punctuation: "12.", "#3", "(4)".
bool isOnlyNumbering(QStringView text)
{
    QStringView rest = text;
    while (!rest.isEmpty() && (rest.front() == u'#' || rest.front() == u'(' || rest.front() == u'[')) {
        rest = rest.sliced(1);
    }
    while (!rest.isEmpty() && (isNumberingPunctuation(rest.back()) || rest.back() == u')')) {
        rest.chop(1);
    }
    return detail::isDigits(rest) && rest.size() <= 3;
}

// "“Glue”", "\"Glue\"" → "Glue".
QString unquote(const QString &text)
{
    const bool straight = text.size() >= 2 && text.front() == u'"' && text.back() == u'"';
    const bool curly = text.size() >= 2 && text.front() == u'“' && text.back() == u'”';
    return straight || curly ? text.sliced(1, text.size() - 2).trimmed() : text;
}

bool isUnidentified(QStringView text)
{
    const QString core = Normalizer::normalize(VersionClassifier::decompose(text).core);
    return core.isEmpty() || core == u"id" || core == u"unknown" || core == u"unknown title" || core == u"tba";
}

bool isUnknownArtist(QStringView text)
{
    const QString key = Normalizer::normalize(text, FieldKind::Artist);
    return key == u"id" || key == u"unknown" || key == u"unknown artist";
}

// The first spaced dash outside brackets.
qsizetype findSeparator(QStringView text)
{
    int depth = 0;
    for (qsizetype i = 0; i + 2 < text.size(); ++i) {
        if (detail::isOpenBracket(text[i])) {
            ++depth;
        } else if (detail::isCloseBracket(text[i]) && depth > 0) {
            --depth;
        } else if (depth == 0 && text[i] == u' ' && text[i + 1] == u'-' && text[i + 2] == u' ') {
            return i;
        }
    }
    return -1;
}

// Artist "Title" (one quoted span; straight or curly quotes).
bool splitQuoted(QStringView text, QString &artist, QString &title)
{
    qsizetype open = text.indexOf(u'"');
    qsizetype close = open >= 0 ? text.indexOf(u'"', open + 1) : -1;
    if (open < 0) {
        open = text.indexOf(u'“');
        close = open >= 0 ? text.indexOf(u'”', open + 1) : -1;
    }
    if (open < 0 || close <= open + 1) {
        return false;
    }
    const QStringView inside = text.sliced(open + 1, close - open - 1).trimmed();
    QString outside = QString(text.first(open).toString() + u' ' + text.sliced(close + 1)).simplified();
    while (outside.endsWith(u'-') || outside.endsWith(u':')) {
        outside.chop(1);
        outside = outside.trimmed();
    }
    if (inside.isEmpty() || outside.isEmpty()) {
        return false;
    }
    artist = std::move(outside);
    title = inside.toString();
    return true;
}

TracklistLine parseLineWith(QStringView line, bool bareNumbers)
{
    TracklistLine result;
    const QStringView bounded =
        line.size() > Normalizer::kMaxFieldLength ? line.first(Normalizer::kMaxFieldLength) : line;
    result.raw = bounded.toString();
    const QString unified = unifyDashes(bounded);
    QStringView text = QStringView(unified).trimmed();
    if (text.isEmpty() || isLink(text)) {
        return result;
    }
    // Leading decorations, in any order and combination.
    if (text.startsWith(u"w/ ", Qt::CaseInsensitive)) {
        result.mashup = true; // a mashup layer line (1001tracklists)
        text = text.sliced(3).trimmed();
    }
    for (int round = 0; round < 6; ++round) {
        // Timestamps before numbering: "12:34" is not track 12.
        const bool stripped = stripBullet(text) || stripTimestamp(text, result.timestampSec) ||
                              stripNumbering(text, bareNumbers) || stripTrackWord(text) || stripVinylPosition(text);
        if (!stripped) {
            break;
        }
    }
    stripTrailingLength(text, result.durationSec);
    stripLabel(text, result.labelHint);
    if (isHeading(text) || isOnlyNumbering(text)) {
        return result;
    }
    result.mashup = result.mashup || text.contains(u" w/ ", Qt::CaseInsensitive);

    QString artist;
    QString title;
    const qsizetype tab = text.indexOf(u'\t');
    const qsizetype dash = findSeparator(text);
    if (tab > 0) {
        const QList<QStringView> columns = text.split(u'\t', Qt::SkipEmptyParts);
        if (columns.size() >= 2) {
            artist = columns.at(0).trimmed().toString();
            title = columns.at(1).trimmed().toString();
        } else {
            title = text.trimmed().toString();
        }
    } else if (dash > 0) {
        artist = text.first(dash).trimmed().toString();
        title = text.sliced(dash + 3).trimmed().toString();
    } else if (!splitQuoted(text, artist, title)) {
        title = text.toString().simplified();
    }
    if (isUnknownArtist(artist)) {
        artist.clear();
    }
    title = unquote(title);
    result.kind = isUnidentified(title) ? TracklistLineKind::Unidentified : TracklistLineKind::Track;
    result.artist = std::move(artist);
    result.title = std::move(title);
    result.keys = Normalizer::trackKeys(result.artist, result.title);
    return result;
}

// Bare "1 Artist - Title" numbering counts only when most lines carry it in sequence (otherwise "808 State" and
// "50 Cent" would lose their numbers).
bool usesBareNumbers(const QList<QStringView> &lines)
{
    int numbered = 0;
    int content = 0;
    int previous = 0;
    int increasing = 0;
    for (const QStringView line : lines) {
        const QStringView text = line.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        ++content;
        const qsizetype digits = digitRun(text, 0, 3);
        if (digits == 0 || digits > 3 || digits >= text.size() || !text[digits].isSpace()) {
            continue;
        }
        ++numbered;
        const int number = text.first(digits).toInt();
        increasing += number > previous ? 1 : 0;
        previous = number;
    }
    return numbered >= 3 && numbered * 10 >= content * 6 && increasing * 10 >= numbered * 8;
}

} // namespace

TracklistLine TracklistParser::parseLine(QStringView line)
{
    return parseLineWith(line, /*bareNumbers=*/false);
}

QList<TracklistLine> TracklistParser::parse(QStringView text)
{
    QList<QStringView> lines;
    qsizetype start = 0;
    for (qsizetype i = 0; i <= text.size() && lines.size() < kMaxLines; ++i) {
        if (i == text.size() || text[i] == u'\n' || text[i] == u'\r') {
            if (i > start) {
                lines.append(text.sliced(start, i - start));
            }
            start = i + 1;
        }
    }
    const bool bareNumbers = usesBareNumbers(lines);
    QList<TracklistLine> result;
    QHash<QString, bool> seen;
    for (const QStringView line : std::as_const(lines)) {
        TracklistLine parsed = parseLineWith(line, bareNumbers);
        if (parsed.kind == TracklistLineKind::Empty) {
            continue;
        }
        const QString key =
            parsed.kind == TracklistLineKind::Track
                ? parsed.keys.exactKey
                : u"unidentified␟"_s + Normalizer::normalize(QString(parsed.artist + u' ' + parsed.title));
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key, true);
        result.append(std::move(parsed));
    }
    return result;
}

} // namespace selecta::match
