// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/RemotePathParser.h"

#include "match/ArtistDictionary.h"
#include "match/Lexicon.h"
#include "match/Normalizer.h"
#include "match/Similarity.h"
#include "match/VersionClassifier.h"
#include "match/detail/RemoteName.h"
#include "match/detail/Text.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace lexicon = detail::lexicon;

namespace {

// --- Confidence of each rule (initial values; to calibrate on recorded results, experiment E3/E8) ---------------
constexpr float kConfidenceSequence = 0.95F;
constexpr float kConfidenceStrong = 0.9F;
constexpr float kConfidenceGood = 0.85F;
constexpr float kConfidenceMedium = 0.8F;
constexpr float kConfidenceFolder = 0.75F;
constexpr float kConfidenceWeak = 0.7F;
constexpr float kConfidenceFallback = 0.4F;
constexpr float kAmbiguityFactor = 0.8F;

// --- Artist/title order evidence, as log-odds (note §17.5 A) ---------------------------------------------------
constexpr double kOrderPrior = 1.4; // "Artist - Title" ≈ 4:1
constexpr double kEvidenceSiblingConstant = 3.0;
constexpr double kEvidenceFolderArtist = 2.0;
constexpr double kEvidenceKnownArtist = 2.5; // plus up to 1.0 for the artist's weight
constexpr double kEvidenceKnownMember = 1.5;
constexpr double kEvidenceKnownFuzzy = 1.2;
constexpr double kEvidenceReleaseArtist = 2.5;
constexpr double kEvidenceTitleFeatures = 1.0;
constexpr double kAmbiguousMargin = 1.0; // |score| below this: both orders are plausible

constexpr int kMinConstantSiblings = 3;

// --- Small text helpers
// -----------------------------------------------------------------------------------------------

bool sameKey(QStringView a, QStringView b, FieldKind kind = FieldKind::Plain)
{
    const QString ka = Normalizer::squash(Normalizer::normalize(a, kind));
    return !ka.isEmpty() && ka == Normalizer::squash(Normalizer::normalize(b, kind));
}

// --- Folders -----------------------------------------------------------------------------------------------------

struct FolderName
{
    QString raw;
    detail::CleanName clean;
    bool generic = false;
    int disc = 0;
    QString discTitle;
};

bool isShareRoot(QStringView raw)
{
    return raw.startsWith(u"@@") || (raw.size() == 2 && raw[1] == u':' && raw[0].isLetter());
}

FolderName analyseFolder(QStringView raw)
{
    FolderName folder;
    folder.raw = raw.trimmed().toString();
    folder.clean = detail::cleanName(folder.raw);
    if (const auto disc = detail::discNumberOf(folder.raw)) {
        folder.disc = *disc;
        return folder;
    }
    // "CD1 - Title", "Disc 2 - Bonus"
    if (const qsizetype dash = folder.raw.indexOf(u" - "); dash > 0) {
        if (const auto disc = detail::discNumberOf(QStringView(folder.raw).first(dash))) {
            folder.disc = *disc;
            folder.discTitle = QStringView(folder.raw).sliced(dash + 3).trimmed().toString();
            return folder;
        }
    }
    // Lower-cased too: CamelCase splitting would turn "iTunes Media" into "i tunes media".
    folder.generic = isShareRoot(folder.raw) || lexicon::isGenericFolder(Normalizer::normalize(folder.raw)) ||
                     lexicon::isGenericFolder(Normalizer::normalize(folder.raw.toLower())) ||
                     Normalizer::normalize(folder.clean.text).isEmpty();
    return folder;
}

// "Artist-Album-WEB-2019-GRP": no spaces, hyphen-separated, with a source tag or a year among the parts.
struct SceneName
{
    QString artist;
    QString album;
    int year = 0;
    QString source;
};

std::optional<SceneName> parseSceneName(QStringView raw)
{
    if (raw.contains(u' ') || raw.count(u'-') < 2) {
        return std::nullopt;
    }
    const QList<QStringView> parts = raw.split(u'-', Qt::SkipEmptyParts);
    qsizetype tag = -1;
    SceneName scene;
    for (qsizetype i = 1; i < parts.size(); ++i) {
        const QString lower = parts.at(i).toString().toLower();
        const QStringView source = lexicon::sceneSource(lower);
        const bool year = detail::isYearToken(lower);
        const bool quality = lexicon::qualityWord(lower) == lexicon::QualityWord::Strong;
        if (!source.isEmpty() || year || quality) {
            if (tag < 0) {
                tag = i;
            }
            if (!source.isEmpty() && scene.source.isEmpty()) {
                scene.source = source.toString();
            }
            if (year && scene.year == 0) {
                scene.year = lower.toInt();
            }
        }
    }
    if (tag < 1) {
        return std::nullopt;
    }
    const auto words = [](QStringView part) { return part.toString().replace(u'_', u' ').trimmed(); };
    scene.artist = words(parts.front());
    QStringList album;
    for (qsizetype i = 1; i < tag; ++i) {
        album.append(words(parts.at(i)));
    }
    scene.album = album.join(u' ');
    return scene;
}

// --- Order evidence ------------------------------------------------------------------------------------------------

double dictionaryEvidence(const ArtistDictionary *dictionary, QStringView segment, double exactStrength)
{
    if (dictionary == nullptr || dictionary->isEmpty()) {
        return 0.0;
    }
    const ArtistDictionary::Hit hit = dictionary->lookup(segment);
    if (!hit.found()) {
        return 0.0;
    }
    if (!hit.exact()) {
        return kEvidenceKnownFuzzy;
    }
    if (hit.memberOnly) {
        return kEvidenceKnownMember;
    }
    return exactStrength + std::min(1.0, 0.5 * std::log10(1.0 + hit.weight));
}

bool hasTitleFeatures(QStringView segment)
{
    const TitleParts parts = VersionClassifier::decompose(segment);
    return parts.version.isExplicit() || parts.version.remaster || !parts.extras.isEmpty() ||
           (!parts.featured.isEmpty() && segment.contains(u'('));
}

struct OrderDecision
{
    bool swapped = false;
    bool ambiguous = false;
    ParseRule rule = ParseRule::OrderPrior;
};

struct OrderEvidence
{
    double score = kOrderPrior; // > 0: the first segment is the artist
    double strongest = 0.0;
    ParseRule rule = ParseRule::OrderPrior;

    void add(double value, ParseRule evidenceRule)
    {
        score += value;
        if (std::abs(value) > std::abs(strongest)) {
            strongest = value;
            rule = evidenceRule;
        }
    }
};

OrderDecision decideOrder(const OrderEvidence &evidence)
{
    OrderDecision decision;
    decision.swapped = evidence.score < 0.0;
    decision.ambiguous = std::abs(evidence.score) < kAmbiguousMargin;
    // The rule that fired is the strongest evidence agreeing with the decision; else the prior.
    const bool agrees = (evidence.strongest < 0.0) == decision.swapped && evidence.strongest != 0.0;
    decision.rule = agrees ? evidence.rule : ParseRule::OrderPrior;
    return decision;
}

// --- Sibling facts (one folder) --------------------------------------------------------------------------------

struct NameInfo
{
    QString fileName;
    QString extension;
    FileKind kind = FileKind::Other;
    detail::CleanName clean;
    std::optional<detail::Prefix> prefix;
    std::optional<int> rip;
};

struct FolderFacts
{
    bool trackSequence = false;                  // numbered siblings
    bool hundreds = false;                       // 101…1nn, 201…2nn
    bool vinyl = false;                          // A1, B2…
    bool scene = false;                          // file names in scene style
    QList<QStringList> segmentsByFile;           // per sampled file (empty for non-audio files)
    QList<QStringList> squashedByFile;           // the same segments as squashed keys
    QHash<qsizetype, QList<qsizetype>> constant; // segment count → indexes constant across ≥ 3 such files

    QList<qsizetype> constantSegments(qsizetype count) const { return constant.value(count); }
};

QString extensionOf(QStringView fileName)
{
    const qsizetype dot = fileName.lastIndexOf(u'.');
    if (dot <= 0 || fileName.size() - dot - 1 > 5 || fileName.size() - dot - 1 < 1) {
        return {};
    }
    const QStringView extension = fileName.sliced(dot + 1);
    if (!std::ranges::all_of(extension, [](QChar c) { return c.isLetterOrNumber() && c.unicode() < 0x80; })) {
        return {};
    }
    return extension.toString().toLower();
}

NameInfo analyseName(QStringView fileName)
{
    NameInfo info;
    info.fileName = fileName.toString();
    info.extension = extensionOf(fileName);
    info.kind = RemotePathParser::fileKind(info.extension);
    const QStringView body =
        info.extension.isEmpty() ? fileName : fileName.first(fileName.size() - info.extension.size() - 1);
    info.clean = detail::cleanName(body);
    info.prefix = detail::scanPrefix(info.clean.text);
    info.rip = detail::ripNumber(info.clean.text);
    return info;
}

bool isHundredsSequence(const QList<int> &numbers)
{
    if (numbers.size() < 2) {
        return false;
    }
    QHash<int, QList<int>> byDisc;
    for (const int number : numbers) {
        if (number < 101 || number > 999 || number % 100 == 0) {
            return false;
        }
        byDisc[number / 100].append(number % 100);
    }
    return std::ranges::all_of(byDisc, [](const QList<int> &tracks) { return tracks.contains(1); });
}

FolderFacts folderFacts(const QList<NameInfo> &names)
{
    FolderFacts facts;
    QList<int> numbers;
    qsizetype audio = 0;
    qsizetype vinyl = 0;
    qsizetype threeDigits = 0;
    qsizetype scene = 0;
    for (const NameInfo &name : names) {
        if (name.kind != FileKind::Audio) {
            continue;
        }
        ++audio;
        const QStringView raw = QStringView(name.fileName);
        if (!raw.contains(u' ') && raw.count(u'-') >= 2) {
            ++scene;
        }
        if (!name.prefix) {
            continue;
        }
        if (!name.prefix->vinyl.isEmpty()) {
            ++vinyl;
        } else if (name.prefix->disc == 0) {
            numbers.append(name.prefix->track);
            threeDigits += name.prefix->digits == 3 ? 1 : 0;
        } else {
            numbers.append((name.prefix->disc * 100) + name.prefix->track);
        }
    }
    if (audio == 0) {
        return facts;
    }
    const auto distinct = [](QList<int> values) {
        std::ranges::sort(values);
        return std::ranges::unique(values).begin() - values.begin();
    };
    facts.trackSequence =
        numbers.size() >= 2 && numbers.size() * 10 >= audio * 6 && distinct(numbers) * 10 >= numbers.size() * 8;
    facts.hundreds = facts.trackSequence && threeDigits == numbers.size() && isHundredsSequence(numbers);
    facts.vinyl = vinyl >= 2 && vinyl * 2 >= audio;
    facts.scene = scene * 2 > audio;
    return facts;
}

// Indexes of segments that are the same in every file with the same number of segments (at least 3 such files).
void computeConstants(FolderFacts &facts)
{
    QHash<qsizetype, QList<const QStringList *>> byCount;
    for (const QStringList &segments : std::as_const(facts.squashedByFile)) {
        if (segments.size() >= 2) {
            byCount[segments.size()].append(&segments);
        }
    }
    for (auto it = byCount.cbegin(); it != byCount.cend(); ++it) {
        const QList<const QStringList *> &same = it.value();
        if (same.size() < kMinConstantSiblings) {
            continue;
        }
        QList<qsizetype> constant;
        for (qsizetype i = 0; i < it.key(); ++i) {
            const QString &first = same.front()->at(i);
            const bool allEqual = !first.isEmpty() && std::ranges::all_of(same, [i, &first](const QStringList *other) {
                return other->at(i) == first;
            });
            if (allEqual) {
                constant.append(i);
            }
        }
        facts.constant.insert(it.key(), constant);
    }
}

// --- The folder context ----------------------------------------------------------------------------------------------

struct FolderContext
{
    QList<FolderName> folders; // top first
    qsizetype albumIndex = -1;
    qsizetype artistIndex = -1;
    int disc = 0;
    QString album;
    QString albumArtist;
    ParseRule albumRule = ParseRule::None;
    ParseRule artistRule = ParseRule::None;
    float albumConfidence = 0.0F;
    float artistConfidence = 0.0F;
    int year = 0;
    QString catalogue;
    QString quality;
    QString source;
    bool variousArtists = false;
    bool scene = false;
    bool vinylHint = false;
    QString parentZone;
    QStringList higherZones;
    QList<ParseRule> rules;
};

bool isVariousName(QStringView text)
{
    return lexicon::isVariousArtists(Normalizer::normalize(text));
}

void takeFolderTags(FolderContext &context, const detail::CleanName &clean)
{
    if (context.year == 0) {
        context.year = clean.year;
    }
    if (context.catalogue.isEmpty()) {
        context.catalogue = clean.catalogue;
    }
    if (context.quality.isEmpty()) {
        context.quality = clean.quality;
    }
    if (context.source.isEmpty()) {
        context.source = clean.source;
    }
    context.vinylHint = context.vinylHint || clean.vinylHint;
}

// The album folder's name: "Artist - Album", "Album - Artist" (reversed by evidence), "Artist - Year - Album",
// "VA - Album", scene names, or just "Album" with the artist from the folder above.
void parseAlbumFolder(FolderContext &context, const FolderName &folder, ParseContext parseContext,
                      const QStringList &fileArtists)
{
    takeFolderTags(context, folder.clean);
    if (const auto scene = parseSceneName(folder.raw)) {
        context.scene = true;
        context.album = scene->album;
        context.albumArtist = scene->artist;
        context.albumRule = ParseRule::SceneFolder;
        context.artistRule = ParseRule::SceneFolder;
        context.albumConfidence = kConfidenceFolder;
        context.artistConfidence = kConfidenceFolder;
        context.year = context.year == 0 ? scene->year : context.year;
        context.source = context.source.isEmpty() ? scene->source : context.source;
        context.variousArtists = isVariousName(scene->artist);
        detail::addRule(context.rules, ParseRule::SceneFolder);
        return;
    }
    QStringList segments = detail::splitTopLevel(folder.clean.text, u" - ");
    // "Artist - 2019 - Album" and "Artist - Album - 2019"
    for (qsizetype i = 0; i < segments.size() && segments.size() > 2; ++i) {
        if (detail::isYearToken(segments.at(i).trimmed())) {
            context.year = context.year == 0 ? segments.at(i).trimmed().toInt() : context.year;
            segments.removeAt(i);
            detail::addRule(context.rules, ParseRule::YearTag);
            break;
        }
    }
    if (segments.size() >= 2 && isVariousName(segments.front())) {
        context.variousArtists = true;
        context.album = segments.mid(1).join(u" - ");
        context.albumRule = ParseRule::VariousArtistsFolder;
        context.albumConfidence = kConfidenceMedium;
        detail::addRule(context.rules, ParseRule::VariousArtistsFolder);
        return;
    }
    if (segments.size() >= 2) {
        const QString &first = segments.front();
        const QString rest = segments.mid(1).join(u" - ");
        OrderEvidence evidence;
        evidence.add(dictionaryEvidence(parseContext.knownArtists, first, kEvidenceKnownArtist) -
                         dictionaryEvidence(parseContext.knownArtists, rest, kEvidenceKnownArtist),
                     ParseRule::OrderKnownArtist);
        evidence.add(dictionaryEvidence(parseContext.releaseArtists, first, kEvidenceReleaseArtist) -
                         dictionaryEvidence(parseContext.releaseArtists, rest, kEvidenceReleaseArtist),
                     ParseRule::OrderReleaseArtist);
        // The files' artist segments agree with one side.
        const QString firstKey = Normalizer::squash(Normalizer::normalize(first, FieldKind::Artist));
        const QString restKey = Normalizer::squash(Normalizer::normalize(rest, FieldKind::Artist));
        const auto agreeFirst = std::ranges::count(fileArtists, firstKey);
        const auto agreeRest = std::ranges::count(fileArtists, restKey);
        if (agreeFirst != agreeRest) {
            evidence.add(agreeFirst > agreeRest ? kEvidenceFolderArtist : -kEvidenceFolderArtist,
                         ParseRule::OrderFolderArtist);
        }
        const OrderDecision order = decideOrder(evidence);
        context.albumArtist = order.swapped ? rest : first;
        context.album = order.swapped ? first : rest;
        context.albumRule = order.swapped ? ParseRule::AlbumArtistFolder : ParseRule::ArtistAlbumFolder;
        context.artistRule = context.albumRule;
        const float confidence = order.swapped ? kConfidenceFolder : kConfidenceMedium;
        context.albumConfidence = order.ambiguous ? confidence * kAmbiguityFactor : confidence;
        context.artistConfidence = context.albumConfidence;
        context.variousArtists = isVariousName(context.albumArtist);
        detail::addRule(context.rules, context.albumRule);
        return;
    }
    context.album = folder.clean.text;
    context.albumRule = ParseRule::AlbumFolder;
    context.albumConfidence = kConfidenceWeak;
    detail::addRule(context.rules, ParseRule::AlbumFolder);
}

FolderContext folderContext(QStringView folderPath, ParseContext parseContext, const QStringList &fileArtists)
{
    FolderContext context;
    const QChar separator = folderPath.contains(u'\\') ? u'\\' : u'/';
    QList<QStringView> parts = folderPath.split(separator, Qt::SkipEmptyParts);
    if (parts.size() > RemotePathParser::kMaxFolders) {
        parts = parts.last(RemotePathParser::kMaxFolders);
    }
    for (const QStringView part : std::as_const(parts)) {
        context.folders.append(analyseFolder(part));
    }
    // Deepest first: disc folders, then the album folder, then the artist folder.
    qsizetype i = context.folders.size() - 1;
    while (i >= 0 && context.folders.at(i).disc > 0) {
        if (context.disc == 0) {
            context.disc = context.folders.at(i).disc;
            detail::addRule(context.rules, ParseRule::DiscFolder);
        }
        --i;
    }
    while (i >= 0 && context.folders.at(i).generic) {
        if (Normalizer::normalize(context.folders.at(i).raw) == u"compilations") {
            context.variousArtists = true; // iTunes' layout
            detail::addRule(context.rules, ParseRule::VariousArtistsFolder);
        }
        --i;
    }
    if (i < 0) {
        return context;
    }
    context.albumIndex = i;
    const FolderName &albumFolder = context.folders.at(i);
    parseAlbumFolder(context, albumFolder, parseContext, fileArtists);
    context.parentZone = albumFolder.clean.plain;
    if (isVariousName(albumFolder.clean.text)) {
        context.variousArtists = true;
        context.album.clear();
        context.albumRule = ParseRule::None;
        context.albumConfidence = 0.0F;
    }
    // The artist folder: the next non-generic folder, when the album folder didn't name the artist.
    for (qsizetype j = i - 1; j >= 0; --j) {
        const FolderName &folder = context.folders.at(j);
        if (folder.generic || folder.disc > 0) {
            if (Normalizer::normalize(folder.raw) == u"compilations") {
                context.variousArtists = true;
                detail::addRule(context.rules, ParseRule::VariousArtistsFolder);
            }
            continue;
        }
        if (context.higherZones.size() < 4) {
            context.higherZones.append(folder.clean.plain);
        }
        if (context.artistIndex < 0 && context.albumArtist.isEmpty() && !context.variousArtists) {
            context.artistIndex = j;
            if (isVariousName(folder.clean.text)) {
                context.variousArtists = true;
            } else {
                context.albumArtist = folder.clean.text;
                context.artistRule = ParseRule::ArtistFolder;
                context.artistConfidence = kConfidenceFolder;
                detail::addRule(context.rules, ParseRule::ArtistFolder);
            }
        }
    }
    return context;
}

// --- One file --------------------------------------------------------------------------------------------------

struct Segmented
{
    QStringList segments;
    int midTrack = 0;
    bool versionMerged = false;
};

bool hasVersionKeyword(QStringView phrase)
{
    const QStringList tokens = Normalizer::tokens(phrase);
    return std::ranges::any_of(tokens, [](const QString &token) {
        const lexicon::VersionWord word = lexicon::versionWord(token);
        return word != lexicon::VersionWord::None && word != lexicon::VersionWord::ClassWord;
    });
}

// Splits the remainder of a name into segments: spaced dashes (bare hyphens for scene names), mid-name track numbers
// removed, a trailing version segment merged into the title.
Segmented segment(QStringView remainder, bool scene, bool sequence)
{
    Segmented result;
    result.segments = detail::splitTopLevel(remainder, u" - ");
    if (scene && result.segments.size() == 1) {
        QStringList parts = detail::splitTopLevel(remainder, u"-"); // scene names: "artist-title name"
        if (parts.size() >= 2) {
            result.segments = std::move(parts);
        }
    }
    // A segment that is only a track number, or starts with one ("Artist - 03 - Title", "Artist - Album - 01 Title").
    for (qsizetype i = 1; i < result.segments.size(); ++i) {
        const QString &segmentText = result.segments.at(i);
        if (detail::isDigits(segmentText) && segmentText.size() <= 3) {
            result.midTrack = segmentText.toInt();
            result.segments.removeAt(i);
            break;
        }
        const auto prefix = detail::scanPrefix(segmentText);
        if (prefix && prefix->vinyl.isEmpty() && prefix->rest &&
            (prefix->leadingZero || prefix->punctuation || sequence)) {
            result.midTrack = prefix->track;
            result.segments[i] = segmentText.sliced(prefix->length).trimmed();
            break;
        }
    }
    // "Artist - Song - Extended Mix", "Song - Original Mix": the version belongs to the title. Only a bare phrase can
    // be a version segment: "Glue (Original Mix)" is a title with its version.
    if (result.segments.size() >= 2) {
        const QString &last = result.segments.back();
        const bool bare = std::ranges::none_of(last, [](QChar c) { return detail::isOpenBracket(c); });
        const bool version = bare && VersionClassifier::classify(last).has_value();
        if (version && (result.segments.size() >= 3 || hasVersionKeyword(last))) {
            const QString merged = result.segments.at(result.segments.size() - 2) + u" - " + last;
            result.segments.removeLast();
            result.segments.back() = merged;
            result.versionMerged = true;
        }
    }
    return result;
}

void splitFeatured(QString &artist, QStringList &featured)
{
    if (const qsizetype marker = detail::findFeatMarker(artist); marker >= 0) {
        const QStringView rest = QStringView(artist).sliced(marker + 1);
        featured.append(rest.sliced(detail::featMarkerLength(rest, false)).trimmed().toString());
        artist.truncate(marker);
        artist = artist.trimmed();
    }
}

struct FileParse
{
    ParsedRemote parsed;
    QStringList segments;
};

void setField(ParsedField<QString> &field, QString value, float confidence, ParseRule rule)
{
    field.value = std::move(value);
    field.confidence = confidence;
    field.rule = rule;
}

void setField(ParsedField<int> &field, int value, float confidence, ParseRule rule)
{
    field.value = value;
    field.confidence = confidence;
    field.rule = rule;
}

// Numbering of one file (note §4.1 step 4): the remainder of the name, and whether a prefix was stripped.
struct Numbered
{
    QString remainder;
    bool stripped = false;
};

bool knownArtist(ParseContext context, QStringView name)
{
    const auto known = [name](const ArtistDictionary *dictionary) {
        return dictionary != nullptr && dictionary->lookup(name).exact();
    };
    return known(context.knownArtists) || known(context.releaseArtists);
}

// "101 Artist - Title" with no siblings: the number is a track number when "Artist" is a known artist and "101 Artist"
// isn't ("808 State" stays whole either way unless the dictionary says otherwise).
bool dictionaryStripsNumber(const QString &text, const detail::Prefix &prefix, ParseContext context)
{
    const QStringList withNumber = detail::splitTopLevel(text, u" - ");
    const QStringList withoutNumber = detail::splitTopLevel(QStringView(text).sliced(prefix.length), u" - ");
    if (withNumber.size() < 2 || withoutNumber.size() < 2) {
        return false;
    }
    return knownArtist(context, withoutNumber.front()) && !knownArtist(context, withNumber.front());
}

Numbered applyNumbering(ParsedRemote &parsed, const NameInfo &name, const FolderFacts &facts,
                        const FolderContext &context, ParseContext parseContext)
{
    const QString &text = name.clean.text;
    if (name.rip) {
        setField(parsed.track, *name.rip, kConfidenceStrong, ParseRule::GenericRipName);
        parsed.rules.append(ParseRule::GenericRipName);
        return {{}, true};
    }
    const std::optional<detail::Prefix> &prefix = name.prefix;
    if (!prefix) {
        return {text, false};
    }
    if (!prefix->vinyl.isEmpty()) {
        const bool accept = facts.vinyl || context.vinylHint || name.clean.vinylHint ||
                            (prefix->punctuation && facts.segmentsByFile.size() <= 1);
        if (!accept) {
            return {text, false};
        }
        parsed.vinylPosition = prefix->vinyl;
        parsed.rules.append(ParseRule::VinylPosition);
        return {text.sliced(prefix->length).trimmed(), true};
    }
    if (!prefix->rest) {
        // Only a number: "01.flac" is a track number without a title; "1999.mp3" isn't a track number at all.
        if (prefix->digits <= 3 && (facts.trackSequence || prefix->leadingZero)) {
            setField(parsed.track, prefix->track, kConfidenceStrong, ParseRule::NumberOnlyName);
            parsed.rules.append(ParseRule::NumberOnlyName);
            return {{}, true};
        }
        return {text, false};
    }
    const bool accept = facts.trackSequence || prefix->leadingZero || prefix->punctuation ||
                        dictionaryStripsNumber(text, *prefix, parseContext);
    if (!accept) {
        return {text, false}; // "808 State", "2 Unlimited", "50 Cent"
    }
    float confidence = kConfidenceWeak;
    if (facts.trackSequence) {
        confidence = kConfidenceSequence;
    } else if (prefix->leadingZero) {
        confidence = kConfidenceMedium;
    }
    if (facts.hundreds && prefix->disc == 0) {
        setField(parsed.disc, prefix->track / 100, kConfidenceGood, ParseRule::HundredsDiscTrack);
        setField(parsed.track, prefix->track % 100, kConfidenceGood, ParseRule::HundredsDiscTrack);
        parsed.rules.append(ParseRule::HundredsDiscTrack);
    } else if (prefix->disc > 0) {
        setField(parsed.disc, prefix->disc, kConfidenceGood, ParseRule::DiscTrackPrefix);
        setField(parsed.track, prefix->track, kConfidenceGood, ParseRule::DiscTrackPrefix);
        parsed.rules.append(ParseRule::DiscTrackPrefix);
    } else {
        setField(parsed.track, prefix->track, confidence, ParseRule::TrackPrefix);
        parsed.rules.append(ParseRule::TrackPrefix);
    }
    return {text.sliced(prefix->length).trimmed(), true};
}

OrderEvidence orderEvidence(const QStringList &segments, qsizetype fileIndex, const FolderFacts &facts,
                            const FolderContext &context, ParseContext parseContext)
{
    OrderEvidence evidence;
    const QString &first = segments.at(0);
    const QString &second = segments.at(1);
    // (a) A segment constant across ≥ 3 siblings is the artist.
    const QList<qsizetype> constant = facts.constantSegments(2);
    if (fileIndex >= 0 && facts.segmentsByFile.at(fileIndex).size() == 2) {
        const bool firstConstant = constant.contains(0);
        const bool secondConstant = constant.contains(1);
        if (firstConstant != secondConstant) {
            evidence.add(firstConstant ? kEvidenceSiblingConstant : -kEvidenceSiblingConstant,
                         ParseRule::OrderSiblingConstant);
        }
    }
    // (b) The segment equal to the folder's artist is the artist.
    if (!context.albumArtist.isEmpty() && !context.variousArtists) {
        const bool firstAgrees = sameKey(first, context.albumArtist, FieldKind::Artist);
        const bool secondAgrees = sameKey(second, context.albumArtist, FieldKind::Artist);
        if (firstAgrees != secondAgrees) {
            evidence.add(firstAgrees ? kEvidenceFolderArtist : -kEvidenceFolderArtist, ParseRule::OrderFolderArtist);
        }
    }
    // (c) Known artists: the collection's, and the current release candidates'.
    evidence.add(dictionaryEvidence(parseContext.knownArtists, first, kEvidenceKnownArtist) -
                     dictionaryEvidence(parseContext.knownArtists, second, kEvidenceKnownArtist),
                 ParseRule::OrderKnownArtist);
    evidence.add(dictionaryEvidence(parseContext.releaseArtists, first, kEvidenceReleaseArtist) -
                     dictionaryEvidence(parseContext.releaseArtists, second, kEvidenceReleaseArtist),
                 ParseRule::OrderReleaseArtist);
    // (d) Title-like features mark the title.
    const bool firstTitle = hasTitleFeatures(first);
    const bool secondTitle = hasTitleFeatures(second);
    if (firstTitle != secondTitle) {
        evidence.add(secondTitle ? kEvidenceTitleFeatures : -kEvidenceTitleFeatures, ParseRule::OrderTitleFeatures);
    }
    return evidence;
}

float orderConfidence(ParseRule rule)
{
    switch (rule) {
    case ParseRule::OrderSiblingConstant:
        return kConfidenceSequence;
    case ParseRule::OrderFolderArtist:
    case ParseRule::OrderKnownArtist:
    case ParseRule::OrderReleaseArtist:
        return kConfidenceStrong;
    case ParseRule::OrderTitleFeatures:
        return kConfidenceFolder;
    default:
        return kConfidenceWeak;
    }
}

// Artist and title from the segments (note §4.1 steps 5–6).
void assignFields(ParsedRemote &parsed, const QStringList &segments, qsizetype fileIndex, const FolderFacts &facts,
                  const FolderContext &context, ParseContext parseContext, bool versionMerged)
{
    if (segments.isEmpty()) {
        return;
    }
    const ParseRule layoutRule = facts.scene ? ParseRule::SceneSplit : ParseRule::ArtistTitle;
    if (segments.size() == 1) {
        const bool folderArtist = !context.albumArtist.isEmpty() && !context.variousArtists;
        setField(parsed.title, segments.front(), folderArtist ? kConfidenceFolder : kConfidenceFallback,
                 folderArtist ? ParseRule::TitleOnly : ParseRule::WholeName);
        parsed.rules.append(parsed.title.rule);
        parsed.wholeNameOnly = !folderArtist;
        if (folderArtist) {
            setField(parsed.artist, context.albumArtist, context.artistConfidence, context.artistRule);
        }
        return;
    }
    if (segments.size() >= 3) {
        // Bandcamp: "Artist - Album - Title", the middle segment being the folder's album (or constant).
        const QList<qsizetype> constant = facts.constantSegments(segments.size());
        const bool middleIsAlbum = (!context.album.isEmpty() && sameKey(segments.at(1), context.album)) ||
                                   (constant.contains(0) && constant.contains(1));
        if (middleIsAlbum) {
            setField(parsed.artist, segments.at(0), kConfidenceStrong, ParseRule::ArtistAlbumTitle);
            setField(parsed.album, segments.at(1), kConfidenceStrong, ParseRule::ArtistAlbumTitle);
            setField(parsed.title, segments.mid(2).join(u" - "), kConfidenceStrong, ParseRule::ArtistAlbumTitle);
            parsed.rules.append(ParseRule::ArtistAlbumTitle);
            return;
        }
    }
    const QStringList pair{segments.at(0), segments.mid(1).join(u" - ")};
    const OrderDecision order = decideOrder(orderEvidence(pair, fileIndex, facts, context, parseContext));
    const float layoutConfidence = facts.trackSequence || facts.segmentsByFile.size() >= kMinConstantSiblings
                                       ? kConfidenceStrong
                                       : kConfidenceGood;
    const float artistConfidence = orderConfidence(order.rule) * (order.ambiguous ? kAmbiguityFactor : 1.0F);
    setField(parsed.artist, order.swapped ? pair.at(1) : pair.at(0), artistConfidence, order.rule);
    setField(parsed.title, order.swapped ? pair.at(0) : pair.at(1), layoutConfidence, layoutRule);
    parsed.orderAmbiguous = order.ambiguous;
    parsed.rules.append(layoutRule);
    parsed.rules.append(order.rule);
    if (versionMerged) {
        parsed.rules.append(ParseRule::TitleVersionSegment);
    }
}

ParsedRemote parseFile(const QString &path, const NameInfo &name, qsizetype fileIndex, const FolderFacts &facts,
                       const FolderContext &context, ParseContext parseContext, bool truncated)
{
    ParsedRemote parsed;
    parsed.path = path;
    parsed.fileName = name.fileName;
    parsed.extension = name.extension;
    parsed.kind = name.kind;
    parsed.truncated = truncated;
    if (truncated) {
        parsed.rules.append(ParseRule::PathTruncated);
    }
    parsed.rules.append(context.rules);
    for (const ParseRule rule : name.clean.rules) {
        if (!parsed.rules.contains(rule)) {
            parsed.rules.append(rule);
        }
    }

    // Folder-level fields.
    if (!context.album.isEmpty()) {
        setField(parsed.album, context.album, context.albumConfidence, context.albumRule);
    }
    if (!context.albumArtist.isEmpty() && !context.variousArtists) {
        setField(parsed.albumArtist, context.albumArtist, context.artistConfidence, context.artistRule);
    }
    const int year = name.clean.year != 0 ? name.clean.year : context.year;
    if (year != 0) {
        setField(parsed.year, year, kConfidenceMedium, ParseRule::YearTag);
    }
    if (context.disc > 0) {
        setField(parsed.disc, context.disc, kConfidenceStrong, ParseRule::DiscFolder);
    } else if (name.clean.disc > 0) {
        setField(parsed.disc, name.clean.disc, kConfidenceMedium, ParseRule::DiscFolder);
    }
    parsed.catalogue = !name.clean.catalogue.isEmpty() ? name.clean.catalogue : context.catalogue;
    parsed.qualityHint = !name.clean.quality.isEmpty() ? name.clean.quality : context.quality;
    parsed.sourceHint = !name.clean.source.isEmpty() ? name.clean.source : context.source;
    if (parsed.sourceHint.isEmpty() && context.vinylHint) {
        parsed.sourceHint = u"VINYL"_s;
    }
    parsed.variousArtists = context.variousArtists;
    parsed.zones.parent = context.parentZone;
    parsed.zones.higher = context.higherZones;
    parsed.zones.brackets = name.clean.brackets;

    if (name.kind != FileKind::Audio) {
        parsed.unparseable = true;
        parsed.keys = Normalizer::trackKeys({}, {}, parsed.album.value);
        return parsed;
    }

    const Numbered numbered = applyNumbering(parsed, name, facts, context, parseContext);
    const QString &remainder = numbered.remainder;
    const bool scene = facts.scene || context.scene;
    const Segmented segmented = segment(remainder, scene, facts.trackSequence);
    if (segmented.midTrack > 0 && !parsed.track.isSet()) {
        setField(parsed.track, segmented.midTrack, kConfidenceMedium, ParseRule::MidTrackNumber);
        parsed.rules.append(ParseRule::MidTrackNumber);
    }
    assignFields(parsed, segmented.segments, fileIndex, facts, context, parseContext, segmented.versionMerged);

    // Featured artists out of the artist; the title keeps its brackets (TitleKeys decomposes them).
    QString artist = parsed.artist.value;
    splitFeatured(artist, parsed.featured);
    parsed.artist.value = std::move(artist);
    const TitleParts titleParts = VersionClassifier::decompose(parsed.title.value);
    parsed.featured.append(titleParts.featured);
    parsed.unparseable = Normalizer::normalize(titleParts.core).isEmpty();
    if (parsed.unparseable) {
        parsed.title = {};
    }
    parsed.keys = Normalizer::trackKeys(parsed.artist.value, parsed.title.value, parsed.album.value);
    parsed.version = parsed.keys.title.version;

    // Zone F: the name without numbering and bracket groups (note §17.5 B).
    QString body;
    if (!remainder.isEmpty()) {
        body = name.clean.plain;
        if (numbered.stripped) {
            const auto plainPrefix = detail::scanPrefix(body);
            body = plainPrefix ? body.sliced(plainPrefix->length).trimmed() : body;
        }
    }
    parsed.zones.body = body;
    parsed.zones.segments = segment(body, scene, false).segments;
    return parsed;
}

QStringView lastComponent(QStringView path)
{
    const qsizetype slash = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return slash >= 0 ? path.sliced(slash + 1) : path;
}

// Marks a folder as various artists when the files' artists vary (note §8, Lidarr's rule).
void detectVariousArtists(QList<ParsedRemote> &results)
{
    QHash<QString, qsizetype> counts;
    qsizetype withArtist = 0;
    for (const ParsedRemote &parsed : std::as_const(results)) {
        if (parsed.kind == FileKind::Audio && parsed.artist.isSet() && parsed.artist.rule != ParseRule::ArtistFolder &&
            parsed.artist.rule != ParseRule::ArtistAlbumFolder && parsed.artist.rule != ParseRule::AlbumArtistFolder &&
            parsed.artist.rule != ParseRule::SceneFolder && !parsed.keys.artist.field.squashed.isEmpty()) {
            ++counts[parsed.keys.artist.field.squashed];
            ++withArtist;
        }
    }
    if (withArtist < kMinConstantSiblings) {
        return;
    }
    const qsizetype most = *std::ranges::max_element(counts);
    const bool various = counts.size() * 4 > withArtist * 3 || most * 4 < withArtist;
    if (!various) {
        return;
    }
    for (ParsedRemote &parsed : results) {
        if (!parsed.variousArtists) {
            parsed.variousArtists = true;
            parsed.rules.append(ParseRule::VariousArtistsFiles);
        }
    }
}

} // namespace

FileKind RemotePathParser::fileKind(QStringView extension)
{
    const QString lower = extension.toString().toLower();
    if (lexicon::isAudioExtension(lower)) {
        return FileKind::Audio;
    }
    if (lexicon::isCompanionExtension(lower)) {
        return FileKind::Companion;
    }
    return FileKind::Other;
}

QList<ParsedRemote> RemotePathParser::parseFolder(QStringView folder, const QStringList &files, ParseContext context)
{
    QList<ParsedRemote> results;
    if (files.isEmpty()) {
        return results;
    }
    bool truncated = false;
    QStringView folderPath = folder;
    if (folderPath.size() > kMaxPathLength) {
        folderPath = folderPath.last(kMaxPathLength);
        truncated = true;
        // The first component was cut: drop it.
        const qsizetype separator = std::max(folderPath.indexOf(u'\\'), folderPath.indexOf(u'/'));
        folderPath = separator >= 0 ? folderPath.sliced(separator + 1) : QStringView();
    }

    // Names: analysed once each; at most kMaxSiblings take part in the folder facts.
    QList<NameInfo> names;
    names.reserve(files.size());
    for (const QString &file : files) {
        QStringView fileName = lastComponent(file);
        if (fileName.size() > kMaxPathLength) {
            fileName = fileName.last(kMaxPathLength);
        }
        names.append(analyseName(fileName));
    }
    const QList<NameInfo> sample = names.first(std::min<qsizetype>(names.size(), kMaxSiblings));
    FolderFacts facts = folderFacts(sample);
    for (const NameInfo &name : sample) {
        if (name.kind != FileKind::Audio) {
            facts.segmentsByFile.append(QStringList{});
            facts.squashedByFile.append(QStringList{});
            continue;
        }
        QString remainder = name.clean.text;
        if (name.prefix && name.prefix->vinyl.isEmpty() && name.prefix->rest &&
            (facts.trackSequence || name.prefix->leadingZero || name.prefix->punctuation)) {
            remainder = remainder.sliced(name.prefix->length).trimmed();
        }
        QStringList segments = segment(remainder, facts.scene, facts.trackSequence).segments;
        QStringList squashed;
        squashed.reserve(segments.size());
        for (const QString &segmentText : std::as_const(segments)) {
            squashed.append(Normalizer::squash(Normalizer::normalize(segmentText, FieldKind::Artist)));
        }
        facts.segmentsByFile.append(std::move(segments));
        facts.squashedByFile.append(std::move(squashed));
    }
    computeConstants(facts);
    // The files' candidate artist segments (first and last) help orient an "Album - Artist" folder name.
    QStringList fileArtists;
    for (const QStringList &squashed : std::as_const(facts.squashedByFile)) {
        if (squashed.size() >= 2) {
            fileArtists.append(squashed.front());
            fileArtists.append(squashed.back());
        }
    }
    const FolderContext folderInfo = folderContext(folderPath, context, fileArtists);
    if (folderInfo.scene) {
        facts.scene = true;
    }
    const QString folderPrefix = folderPath.isEmpty() ? QString() : folderPath.toString() + u'\\';
    for (qsizetype i = 0; i < names.size(); ++i) {
        const qsizetype factIndex = i < sample.size() ? i : -1;
        results.append(parseFile(folderPrefix + names.at(i).fileName, names.at(i), factIndex, facts, folderInfo,
                                 context, truncated));
    }
    detectVariousArtists(results);
    return results;
}

ParsedRemote RemotePathParser::parse(QStringView path, const QStringList &siblingNames, ParseContext context)
{
    QStringView bounded = path;
    const bool truncated = bounded.size() > kMaxPathLength;
    if (truncated) {
        bounded = bounded.last(kMaxPathLength);
        // The first component was cut in the middle: drop it.
        const qsizetype first = std::max(bounded.indexOf(u'\\'), bounded.indexOf(u'/'));
        bounded = first >= 0 ? bounded.sliced(first + 1) : bounded;
    }
    const QChar separator = bounded.contains(u'\\') ? u'\\' : u'/';
    const qsizetype cut = bounded.lastIndexOf(separator);
    const QStringView folder = cut >= 0 ? bounded.first(cut) : QStringView();
    const QStringView fileName = cut >= 0 ? bounded.sliced(cut + 1) : bounded;

    QStringList files{fileName.toString()};
    for (const QString &sibling : siblingNames) {
        const QStringView name = lastComponent(sibling);
        if (name != fileName && files.size() <= kMaxSiblings) {
            files.append(name.toString());
        }
    }
    QList<ParsedRemote> results = parseFolder(folder, files, context);
    ParsedRemote parsed = std::move(results.front());
    parsed.path = bounded.toString();
    if (truncated) {
        parsed.truncated = true;
        parsed.rules.prepend(ParseRule::PathTruncated);
    }
    return parsed;
}

} // namespace selecta::match
