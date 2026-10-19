// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/VersionClassifier.h"

#include "match/Lexicon.h"
#include "match/Normalizer.h"
#include "match/detail/Text.h"

#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace lexicon = detail::lexicon;

namespace {

QStringView bounded(QStringView text)
{
    return text.size() > Normalizer::kMaxFieldLength ? text.first(Normalizer::kMaxFieldLength) : text;
}

// A normalised token with the span of the raw words it came from.
struct Unit
{
    QString token;
    qsizetype begin = 0;
    qsizetype end = 0;
};

bool isReSuffix(QStringView token)
{
    static constexpr std::array<QStringView, 8> kSuffixes{u"mix", u"edit", u"work",  u"fix",
                                                          u"rub", u"make", u"model", u"build"};
    return std::ranges::find(kSuffixes, token) != kSuffixes.end();
}

// Two-token spellings joined into one: "re-edit" → "reedit", "full length", "a cappella", "12 inch", "7 inch".
QString joinedToken(QStringView a, QStringView b)
{
    if (a == u"re" && isReSuffix(b)) {
        return a.toString() + b;
    }
    if (a == u"full" && b == u"length") {
        return u"fulllength"_s;
    }
    if (a == u"a" && (b == u"cappella" || b == u"capella")) {
        return u"acapella"_s;
    }
    if ((a == u"12" || a == u"twelve") && b == u"inch") {
        return u"12inch"_s;
    }
    if ((a == u"7" || a == u"seven") && b == u"inch") {
        return u"7inch"_s;
    }
    return {};
}

QList<Unit> units(QStringView phrase)
{
    QList<Unit> raw;
    const QList<QStringView> phraseWords = detail::words(phrase);
    for (const QStringView word : phraseWords) {
        const qsizetype begin = word.data() - phrase.data();
        const qsizetype end = begin + word.size();
        for (QString &token : Normalizer::tokens(word)) {
            raw.append({std::move(token), begin, end});
        }
    }
    QList<Unit> joined;
    joined.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (i + 1 < raw.size()) {
            QString merged = joinedToken(raw.at(i).token, raw.at(i + 1).token);
            if (!merged.isEmpty()) {
                joined.append({std::move(merged), raw.at(i).begin, raw.at(i + 1).end});
                ++i;
                continue;
            }
        }
        joined.append(raw.at(i));
    }
    return joined;
}

void resolveClasses(VersionInfo &info, const QList<VersionClass> &classes)
{
    VersionClass main = VersionClass::Unspecified;
    for (const VersionClass c : classes) {
        if (lexicon::classPrecedence(c) > lexicon::classPrecedence(main)) {
            main = c;
        }
    }
    info.versionClass = main;
    for (const VersionClass c : classes) {
        if (c != main) {
            info.modifiers |= lexicon::modifierFor(c);
        }
    }
}

void setBy(VersionInfo &info, QStringView named)
{
    QString display = named.trimmed().toString();
    if (display.endsWith(u"'s", Qt::CaseInsensitive) || display.endsWith(u"’s", Qt::CaseInsensitive)) {
        display.chop(2); // "Kerri Chandler's Remix" names Kerri Chandler
    }
    info.byDisplay = display.trimmed();
    info.by = Normalizer::artistKeys(info.byDisplay).set;
}

bool isRemixWordBeforeBy(QStringView token)
{
    return token == u"remix" || token == u"remixed" || token == u"rmx" || token == u"rework" || token == u"reworked" ||
           token == u"mix" || token == u"version";
}

bool isEditWordBeforeBy(QStringView token)
{
    return token == u"edit" || token == u"edited" || token == u"reedit";
}

std::optional<VersionClass> unitClass(const Unit &unit)
{
    if (const auto word = lexicon::classWord(unit.token)) {
        return word->versionClass;
    }
    switch (lexicon::versionWord(unit.token)) {
    case lexicon::VersionWord::DubKeyword:
        return VersionClass::Dub;
    case lexicon::VersionWord::VipKeyword:
        return VersionClass::Vip;
    default:
        return std::nullopt;
    }
}

// "remix by X", "remixed by X", "edit by X", "mixed by X" (a DJ-mixed compilation).
std::optional<VersionInfo> classifyBy(QStringView text, const QList<Unit> &all, VersionInfo info)
{
    for (qsizetype b = 1; b + 1 < all.size(); ++b) {
        if (all.at(b).token != u"by") {
            continue;
        }
        const QString &before = all.at(b - 1).token;
        if (before == u"mixed") {
            info.mixed = true;
            return info;
        }
        const bool edit = isEditWordBeforeBy(before);
        if (!edit && !isRemixWordBeforeBy(before)) {
            return std::nullopt;
        }
        info.versionClass = VersionClass::Remix;
        if (edit) {
            info.modifiers |= VersionModifier::Edit;
        }
        for (qsizetype i = 0; i + 1 < b; ++i) {
            if (const auto c = unitClass(all.at(i))) {
                info.modifiers |= lexicon::modifierFor(*c);
            }
        }
        setBy(info, text.sliced(all.at(b + 1).begin));
        return info;
    }
    return std::nullopt;
}

bool allVariantWords(const QList<Unit> &all, qsizetype end)
{
    for (qsizetype i = 0; i < end; ++i) {
        if (!lexicon::isVariantWord(all.at(i).token)) {
            return false;
        }
    }
    return end > 0;
}

std::optional<VersionInfo> classifyKeyword(QStringView text, const QList<Unit> &all, VersionInfo info)
{
    const lexicon::VersionWord keyword = lexicon::versionWord(all.back().token);
    if (keyword == lexicon::VersionWord::None || keyword == lexicon::VersionWord::ClassWord) {
        return std::nullopt;
    }
    qsizetype end = all.size() - 1; // the named part and class words are all[0, end)
    QList<VersionClass> classes;
    while (end > 0) {
        const auto c = unitClass(all.at(end - 1));
        if (!c) {
            break;
        }
        classes.append(*c);
        --end;
    }
    if (end > 0) {
        if (keyword == lexicon::VersionWord::VersionKeyword && allVariantWords(all, end)) {
            return std::nullopt; // "Clean Version", "Spanish Version": a variant, kept as title extra text
        }
        info.versionClass = keyword == lexicon::VersionWord::VipKeyword ? VersionClass::Vip : VersionClass::Remix;
        for (const VersionClass c : std::as_const(classes)) {
            info.modifiers |= lexicon::modifierFor(c);
        }
        if (keyword == lexicon::VersionWord::EditKeyword) {
            info.modifiers |= VersionModifier::Edit;
        } else if (keyword == lexicon::VersionWord::DubKeyword) {
            info.modifiers |= VersionModifier::Dub;
        }
        setBy(info, text.sliced(all.front().begin, all.at(end - 1).end - all.front().begin));
        return info;
    }
    switch (keyword) {
    case lexicon::VersionWord::RemixKeyword:
        info.versionClass = VersionClass::Remix; // "(Remix)", "(Extended Remix)": a remix by someone unnamed
        for (const VersionClass c : std::as_const(classes)) {
            info.modifiers |= lexicon::modifierFor(c);
        }
        return info;
    case lexicon::VersionWord::EditKeyword:
        if (classes.isEmpty()) {
            classes.append(VersionClass::Radio); // an unnamed edit is a radio edit unless a class word says otherwise
        }
        break;
    case lexicon::VersionWord::DubKeyword:
        classes.append(VersionClass::Dub);
        break;
    case lexicon::VersionWord::VipKeyword:
        classes.append(VersionClass::Vip);
        break;
    case lexicon::VersionWord::MixKeyword:
    case lexicon::VersionWord::VersionKeyword:
        if (classes.isEmpty()) {
            return std::nullopt; // "(Mix)", "(Version)"
        }
        break;
    case lexicon::VersionWord::None:
    case lexicon::VersionWord::ClassWord:
        return std::nullopt;
    }
    resolveClasses(info, classes);
    return info;
}

std::optional<VersionInfo> classifyClassWords(const QList<Unit> &all, VersionInfo info)
{
    QList<VersionClass> classes;
    for (const Unit &unit : all) {
        const auto word = lexicon::classWord(unit.token);
        if (word && !word->needsKeyword) {
            classes.append(word->versionClass);
            continue;
        }
        const auto keyword = lexicon::versionWord(unit.token);
        if (keyword == lexicon::VersionWord::DubKeyword) {
            classes.append(VersionClass::Dub);
        } else if (keyword == lexicon::VersionWord::VipKeyword) {
            classes.append(VersionClass::Vip);
        } else {
            return std::nullopt;
        }
    }
    resolveClasses(info, classes);
    return info;
}

std::optional<VersionInfo> classifyLive(QStringView text, const QList<Unit> &all, VersionInfo info)
{
    if (all.size() < 2 || all.front().token != u"live") {
        return std::nullopt;
    }
    qsizetype venue = 1;
    const QString &connector = all.at(1).token;
    if (connector == u"at" || connector == u"in" || connector == u"from" || connector == u"on") {
        venue = 2;
    }
    info.versionClass = VersionClass::Live;
    if (venue < all.size()) {
        info.byDisplay = text.sliced(all.at(venue).begin).trimmed().toString();
        const QString key = Normalizer::normalize(info.byDisplay);
        if (!key.isEmpty()) {
            info.by = QStringList{key};
        }
    }
    return info;
}

// Merges a further version group into `into` (several groups: "(Bicep Remix) (Radio Edit)").
void mergeVersion(VersionInfo &into, const VersionInfo &from)
{
    into.remaster = into.remaster || from.remaster;
    into.mixed = into.mixed || from.mixed;
    into.raw = into.raw.isEmpty() ? from.raw : into.raw + u" / " + from.raw;
    if (!from.isExplicit()) {
        return;
    }
    if (!into.isExplicit()) {
        into.versionClass = from.versionClass;
        into.modifiers = from.modifiers;
        into.by = from.by;
        into.byDisplay = from.byDisplay;
        return;
    }
    VersionInfo higher = into;
    VersionInfo lower = from;
    if (lexicon::classPrecedence(from.versionClass) > lexicon::classPrecedence(into.versionClass)) {
        std::swap(higher, lower);
    }
    into.versionClass = higher.versionClass;
    into.modifiers = higher.modifiers | lower.modifiers;
    if (lower.versionClass != higher.versionClass) {
        into.modifiers |= lexicon::modifierFor(lower.versionClass);
    }
    into.by = higher.by.isEmpty() ? lower.by : higher.by;
    into.byDisplay = higher.by.isEmpty() ? lower.byDisplay : higher.byDisplay;
}

enum class GroupKind : quint8 {
    Junk,
    Feat,
    Version,
    Part,
    Extra,
};

bool isPartGroup(const QStringList &tokens)
{
    return tokens.size() == 2 && lexicon::isNumberingWord(tokens.front()) && detail::isDigits(tokens.at(1));
}

bool isQualityGroup(const QStringList &tokens)
{
    bool strong = false;
    for (const QString &token : tokens) {
        const lexicon::QualityWord word = lexicon::qualityWord(token);
        if (word == lexicon::QualityWord::None) {
            return false;
        }
        strong = strong || word == lexicon::QualityWord::Strong;
    }
    return strong;
}

struct Piece
{
    QString content;
    QChar open; // '(' '[' '{', or 0 for a " - " segment
    GroupKind kind = GroupKind::Extra;
    VersionInfo version;
};

GroupKind classifyGroup(Piece &piece)
{
    const QStringList tokens = Normalizer::tokens(piece.content, FieldKind::Title);
    const QString normalised = tokens.join(u' ');
    if (normalised.isEmpty() || lexicon::isWebJunkPhrase(normalised) || detail::isYearToken(normalised) ||
        isQualityGroup(tokens)) {
        return GroupKind::Junk;
    }
    if (detail::featMarkerLength(piece.content, /*inBrackets=*/true) > 0) {
        return GroupKind::Feat;
    }
    if (auto version = VersionClassifier::classify(piece.content)) {
        piece.version = std::move(*version);
        return GroupKind::Version;
    }
    if (isPartGroup(tokens)) {
        return GroupKind::Part;
    }
    return GroupKind::Extra;
}

// The last opening bracket that is never closed ("Title (Extended Mi"), or -1.
qsizetype lastUnclosedOpen(QStringView text)
{
    int depth = 0;
    for (qsizetype i = text.size() - 1; i >= 0; --i) {
        if (detail::isCloseBracket(text[i])) {
            ++depth;
        } else if (detail::isOpenBracket(text[i])) {
            if (depth == 0) {
                return i;
            }
            --depth;
        }
    }
    return -1;
}

QString trimSeparators(QStringView text)
{
    QStringView view = text.trimmed();
    while (!view.isEmpty() && (view.back() == u'-' || view.back() == u',' || view.back() == u':' ||
                               view.back() == u'/' || view.back().isSpace())) {
        view.chop(1);
    }
    return view.toString();
}

// Peels trailing bracket groups and " - <version>" segments, last first.
QList<Piece> peel(QString &text, bool &truncated)
{
    QList<Piece> pieces;
    static constexpr int kMaxPieces = 16;
    while (pieces.size() < kMaxPieces) {
        const QStringView view = QStringView(text).trimmed();
        if (view.isEmpty()) {
            break;
        }
        if (detail::isCloseBracket(view.back())) {
            const qsizetype open = detail::matchingOpenBracket(view, view.size() - 1);
            if (open < 0) {
                break;
            }
            const QStringView before = view.first(open).trimmed();
            if (before.isEmpty()) {
                break; // the whole rest is one group: it is the core
            }
            pieces.append(
                {view.sliced(open + 1, view.size() - open - 2).trimmed().toString(), view[open], GroupKind::Extra, {}});
            text = before.toString();
            continue;
        }
        if (const qsizetype open = lastUnclosedOpen(view); open > 0) {
            pieces.append({view.sliced(open + 1).trimmed().toString(), view[open], GroupKind::Extra, {}});
            truncated = true;
            text = view.first(open).trimmed().toString();
            continue;
        }
        const qsizetype dash = view.lastIndexOf(u" - ");
        if (dash > 0) {
            const QStringView segment = view.sliced(dash + 3).trimmed();
            if (auto version = VersionClassifier::classify(segment)) {
                Piece piece{segment.toString(), QChar(), GroupKind::Version, std::move(*version)};
                pieces.append(std::move(piece));
                text = view.first(dash).trimmed().toString();
                continue;
            }
        }
        break;
    }
    return pieces;
}

} // namespace

QString VersionInfo::key() const
{
    if (versionClass == VersionClass::Unspecified) {
        return {};
    }
    QString key = versionClassName(versionClass);
    if (!by.isEmpty()) {
        QStringList squashed;
        squashed.reserve(by.size());
        for (const QString &name : by) {
            squashed.append(Normalizer::squash(name));
        }
        squashed.sort();
        squashed.removeDuplicates();
        key += u':' + squashed.join(u',');
    }
    static constexpr std::array<std::pair<VersionModifier, QStringView>, 11> kModifiers{{
        {VersionModifier::Extended, u"extended"},
        {VersionModifier::Radio, u"radio"},
        {VersionModifier::Instrumental, u"instrumental"},
        {VersionModifier::Vocal, u"vocal"},
        {VersionModifier::Dub, u"dub"},
        {VersionModifier::Edit, u"edit"},
        {VersionModifier::Acapella, u"acapella"},
        {VersionModifier::Live, u"live"},
        {VersionModifier::Acoustic, u"acoustic"},
        {VersionModifier::Demo, u"demo"},
        {VersionModifier::Vip, u"vip"},
    }};
    for (const auto &[modifier, name] : kModifiers) {
        if (modifiers.testFlag(modifier)) {
            key += u'+' + name;
        }
    }
    return key;
}

std::optional<VersionInfo> VersionClassifier::classify(QStringView phrase)
{
    const QStringView text = bounded(phrase).trimmed();
    QList<Unit> all = units(text);
    if (all.isEmpty()) {
        return std::nullopt;
    }
    VersionInfo info;
    info.raw = text.toString();

    // "2011 Remaster", "Remastered 2009", "Digitally Remastered", "Remastered Version": a flag, not a version.
    const bool remaster = std::ranges::any_of(all, [](const Unit &u) {
        return lexicon::isRemasterWord(u.token) && u.token != u"digital" && u.token != u"digitally";
    });
    if (remaster) {
        info.remaster = true;
        all.removeIf([](const Unit &u) { return lexicon::isRemasterWord(u.token) || detail::isYearToken(u.token); });
        const bool onlyFiller = std::ranges::all_of(
            all, [](const Unit &u) { return u.token == u"version" || u.token == u"edition" || u.token == u"the"; });
        if (onlyFiller) {
            return info;
        }
    }
    // DJ-mixed compilations: "Mixed", "Mixed by X", "DJ Mix", "Continuous Mix".
    const bool djMix = all.size() == 2 && all.back().token == u"mix" &&
                       (all.front().token == u"dj" || all.front().token == u"continuous");
    if (all.front().token == u"mixed" || djMix) {
        info.mixed = true;
        return info;
    }
    if (auto byVersion = classifyBy(text, all, info)) {
        return byVersion;
    }
    if (auto keywordVersion = classifyKeyword(text, all, info)) {
        return keywordVersion;
    }
    if (auto classVersion = classifyClassWords(all, info)) {
        return classVersion;
    }
    if (auto liveVersion = classifyLive(text, all, info)) {
        return liveVersion;
    }
    return std::nullopt;
}

TitleParts VersionClassifier::decompose(QStringView title)
{
    TitleParts parts;
    QString text = bounded(title).trimmed().toString();
    QList<Piece> pieces = peel(text, parts.truncated);

    // Classify in reading order (pieces were peeled last first).
    std::ranges::reverse(pieces);
    bool versionFound = false;
    for (Piece &piece : pieces) {
        if (piece.open.isNull()) {
            versionFound = true; // a " - <version>" segment, classified while peeling
            continue;
        }
        piece.kind = classifyGroup(piece);
        versionFound = versionFound || piece.kind == GroupKind::Version;
    }
    // A trailing "[Label]" after a version group is a label hint (1001tracklists style), not title text.
    if (versionFound && !pieces.isEmpty() && pieces.back().open == u'[' && pieces.back().kind == GroupKind::Extra) {
        parts.labelHint = pieces.back().content;
        pieces.removeLast();
    }

    QStringList keptInCore;
    for (const Piece &piece : std::as_const(pieces)) {
        switch (piece.kind) {
        case GroupKind::Junk:
            break;
        case GroupKind::Feat:
            parts.featured.append(
                piece.content.sliced(detail::featMarkerLength(piece.content, /*inBrackets=*/true)).trimmed());
            break;
        case GroupKind::Version:
            mergeVersion(parts.version, piece.version);
            break;
        case GroupKind::Part:
            keptInCore.append(piece.content);
            break;
        case GroupKind::Extra:
            parts.extras.append(piece.content);
            break;
        }
    }

    // An unbracketed " feat. X" in the remaining title.
    if (const qsizetype marker = detail::findFeatMarker(text); marker >= 0) {
        const QStringView rest = QStringView(text).sliced(marker + 1);
        parts.featured.append(rest.sliced(detail::featMarkerLength(rest, /*inBrackets=*/false)).trimmed().toString());
        text.truncate(marker);
    }
    QString core = trimSeparators(text);
    for (const QString &kept : std::as_const(keptInCore)) {
        core += u' ' + kept;
    }
    parts.core = core.trimmed();
    return parts;
}

VersionCompatibility VersionClassifier::compare(const VersionInfo &a, const VersionInfo &b)
{
    if (a.key() == b.key()) {
        return VersionCompatibility::Same;
    }
    const auto plainOriginal = [](const VersionInfo &v) {
        return v.versionClass == VersionClass::Original && v.modifiers == VersionModifier::None && v.by.isEmpty();
    };
    if ((!a.isExplicit() && plainOriginal(b)) || (!b.isExplicit() && plainOriginal(a))) {
        return VersionCompatibility::ProbablySame;
    }
    if (!a.isExplicit() || !b.isExplicit()) {
        return VersionCompatibility::Unsure;
    }
    const bool namedClass = a.versionClass == VersionClass::Remix || a.versionClass == VersionClass::Vip ||
                            a.versionClass == VersionClass::Live;
    if (a.versionClass == b.versionClass && a.modifiers == b.modifiers && namedClass &&
        (a.by.isEmpty() != b.by.isEmpty())) {
        return VersionCompatibility::Unsure; // "(Remix)" against "(Bicep Remix)": maybe the same remix
    }
    return VersionCompatibility::OtherVersion;
}

} // namespace selecta::match
