// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/Naming.h"

#include <QList>
#include <QTextBoundaryFinder>

using namespace Qt::StringLiterals;

namespace selecta::library::naming {
namespace {

constexpr int kSanitiseRounds = 3;

QString tokenValue(QStringView name, const Fields &fields)
{
    const QString key = name.toString().toCaseFolded();
    if (key == u"artist") {
        return fields.artist;
    }
    if (key == u"albumartist") {
        return fields.albumArtist;
    }
    if (key == u"title") {
        return fields.title;
    }
    if (key == u"album") {
        return fields.album;
    }
    if (key == u"year") {
        return fields.year;
    }
    if (key == u"track") {
        return fields.track;
    }
    if (key == u"disc") {
        return fields.disc;
    }
    if (key == u"disctotal") {
        return fields.discTotal;
    }
    if (key == u"label") {
        return fields.label;
    }
    if (key == u"catalognumber") {
        return fields.catalogNumber;
    }
    if (key == u"key") {
        return fields.key;
    }
    if (key == u"bpm") {
        return fields.bpm;
    }
    return {};
}

// "7" with width 2 → "07"; anything that isn't a plain number is left alone.
QString padded(const QString &value, int width)
{
    bool numeric = false;
    const int number = value.trimmed().toInt(&numeric);
    if (!numeric || number < 0 || width <= 0) {
        return value;
    }
    return u"%1"_s.arg(number, width, 10, u'0');
}

// Expands pattern[from, to) and reports whether every token in it had a value.
QString expandRange(QStringView pattern, qsizetype from, qsizetype to, const Fields &fields, bool *complete)
{
    QString out;
    qsizetype i = from;
    while (i < to) {
        const QChar c = pattern.at(i);
        if (c == u'{') {
            const qsizetype close = pattern.indexOf(u'}', i + 1);
            if (close < 0 || close >= to) {
                out += pattern.mid(i, to - i);
                break;
            }
            const QStringView spec = pattern.mid(i + 1, close - i - 1);
            const qsizetype colon = spec.indexOf(u':');
            const QStringView name = colon < 0 ? spec : spec.left(colon);
            QString value = tokenValue(name, fields).trimmed();
            if (colon >= 0) {
                value = padded(value, spec.mid(colon + 1).toInt());
            } else if (name.compare(u"track", Qt::CaseInsensitive) == 0) {
                value = padded(value, 2);
            }
            if (value.isEmpty()) {
                *complete = false;
            }
            out += value;
            i = close + 1;
        } else if (c == u'[') {
            // Find the matching bracket, allowing nested sections.
            qsizetype depth = 1;
            qsizetype j = i + 1;
            for (; j < to && depth > 0; ++j) {
                if (pattern.at(j) == u'[') {
                    ++depth;
                } else if (pattern.at(j) == u']') {
                    --depth;
                }
            }
            if (depth != 0) {
                out += pattern.mid(i, to - i); // unbalanced: literal
                break;
            }
            bool sectionComplete = true;
            const QString section = expandRange(pattern, i + 1, j - 1, fields, &sectionComplete);
            if (sectionComplete) {
                out += section;
            }
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

bool isQuote(char32_t c)
{
    switch (c) {
    case U'\'':
    case U'"':
    case U'`':
    case U'\u00B4': // acute accent used as an apostrophe
    case U'\u2018':
    case U'\u2019':
    case U'\u201A':
    case U'\u201B':
    case U'\u201C':
    case U'\u201D':
    case U'\u201E':
    case U'\u201F':
        return true;
    default:
        return false;
    }
}

// Invisible or direction-changing characters: they can hide or fake a name's extension (U+202E).
bool isDangerous(char32_t c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x200B || c == 0x200C || c == 0x200D || c == 0x200E ||
           c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}

QString removeDangerous(const QString &text)
{
    QString out;
    out.reserve(text.size());
    const QList<uint> characters = QStringView(text).toUcs4();
    for (const char32_t c : characters) {
        if (!isDangerous(c)) {
            out += QString::fromUcs4(&c, 1);
        }
    }
    return out;
}

bool isReservedDeviceName(const QString &name)
{
    static const QStringList reserved{u"CON"_s,  u"PRN"_s,  u"AUX"_s,  u"NUL"_s,  u"COM1"_s, u"COM2"_s,
                                      u"COM3"_s, u"COM4"_s, u"COM5"_s, u"COM6"_s, u"COM7"_s, u"COM8"_s,
                                      u"COM9"_s, u"LPT1"_s, u"LPT2"_s, u"LPT3"_s, u"LPT4"_s, u"LPT5"_s,
                                      u"LPT6"_s, u"LPT7"_s, u"LPT8"_s, u"LPT9"_s};
    const QString stem = name.section(u'.', 0, 0).trimmed();
    return reserved.contains(stem, Qt::CaseInsensitive);
}

// Steps 2–6 of §7.2: everything but the length.
QString sanitiseOnce(const QString &input)
{
    QString name = removeDangerous(input);
    name.replace(u'/', u'-');
    name.replace(u'\\', u'-');
    name.replace(u':', u'-');
    for (const QChar reserved : {u'?', u'*', u'"', u'<', u'>', u'|'}) {
        name.replace(reserved, u'_');
    }
    name = name.trimmed();
    while (name.endsWith(u'.') || name.endsWith(u' ')) {
        name.chop(1);
    }
    if (name.startsWith(u'.') || name.startsWith(u'-')) {
        name[0] = u'_';
    }
    if (isReservedDeviceName(name)) {
        // "CON" → "CON_", "nul.txt" → "nul_.txt": the stem is what Windows reserves.
        const qsizetype dot = name.indexOf(u'.');
        name.insert(dot < 0 ? name.size() : dot, u'_');
    }
    return name;
}

// The longest prefix of `text` that fits `maxBytes` in UTF-8, cut at a grapheme boundary.
QString fitBytes(const QString &text, int maxBytes)
{
    if (text.toUtf8().size() <= maxBytes) {
        return text;
    }
    QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, text);
    qsizetype best = 0;
    qsizetype bytes = 0;
    qsizetype previous = 0;
    for (qsizetype next = graphemes.toNextBoundary(); next > 0; next = graphemes.toNextBoundary()) {
        bytes += text.mid(previous, next - previous).toUtf8().size();
        if (bytes > maxBytes) {
            break;
        }
        best = next;
        previous = next;
    }
    return text.left(best);
}

Fields withFallbacks(Fields fields)
{
    const auto fallback = [](QString &value, const QString &replacement) {
        if (value.trimmed().isEmpty()) {
            value = replacement;
        }
    };
    fallback(fields.artist, u"Unknown Artist"_s);
    fallback(fields.albumArtist, fields.artist);
    fallback(fields.title, u"Untitled"_s);
    fallback(fields.album, u"Unknown Album"_s);
    return fields;
}

} // namespace

QString expand(QStringView pattern, const Fields &fields)
{
    bool complete = true;
    return expandRange(pattern, 0, pattern.size(), fields, &complete);
}

QString clean(const QString &name, QChar separator)
{
    const QString normalised = name.normalized(QString::NormalizationForm_C);
    QString text;
    text.reserve(normalised.size());
    const QList<uint> normalisedCharacters = QStringView(normalised).toUcs4();
    for (const char32_t c : normalisedCharacters) {
        if (isQuote(c)) {
            continue;
        }
        if (c == U'&') {
            text += u" and "_s;
            continue;
        }
        text += QString::fromUcs4(&c, 1);
    }

    QString out;
    out.reserve(text.size());
    const QList<uint> textCharacters = QStringView(text).toUcs4();
    for (const char32_t c : textCharacters) {
        const QString character = QString::fromUcs4(&c, 1);
        const QChar first = character.at(0);
        const bool keep =
            QChar::isLetterOrNumber(c) || QChar::isMark(c) || c == U'-' || c == U'.' || first == separator;
        if (keep) {
            out += character;
        } else if (!out.endsWith(separator)) {
            out += separator; // spaces, brackets and symbols, with repeats collapsed as they come
        }
    }
    // Collapse separators that met through kept characters ("a__b" from "a_ _b" can't happen above, but a separator
    // typed in the name next to a replaced symbol can).
    const QString doubled = QString(2, separator);
    while (out.contains(doubled)) {
        out.replace(doubled, QString(separator));
    }
    qsizetype start = 0;
    qsizetype end = out.size();
    while (start < end && (out.at(start) == separator || out.at(start) == u'.')) {
        ++start;
    }
    while (end > start && (out.at(end - 1) == separator || out.at(end - 1) == u'.')) {
        --end;
    }
    return out.mid(start, end - start);
}

QString safeComponent(const QString &name, int maxBytes)
{
    QString result = name.normalized(QString::NormalizationForm_C);
    for (int round = 0; round < kSanitiseRounds; ++round) {
        const QString sanitised = sanitiseOnce(result);
        const QString fitted = fitBytes(sanitised, std::max(1, maxBytes));
        const bool stable = fitted == sanitised && sanitised == result;
        result = fitted;
        if (stable) {
            break;
        }
    }
    result = sanitiseOnce(result);
    if (result.isEmpty() || result == u"." || result == u"..") {
        return u"Untitled"_s;
    }
    return result;
}

QString componentName(QStringView pattern, const Fields &fields, Cleaner cleaner, const QString &extension)
{
    QString base = expand(pattern, withFallbacks(fields));
    if (cleaner.enabled) {
        base = clean(base, cleaner.separator);
    }
    const QString suffix = extension.isEmpty() ? QString() : u"."_s + safeComponent(extension, 16);
    const int room = kMaxComponentBytes - static_cast<int>(suffix.toUtf8().size());
    base = safeComponent(base, std::max(1, room));
    return base + suffix;
}

} // namespace selecta::library::naming
