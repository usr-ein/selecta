// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/Scoring.h"

#include "match/Lexicon.h"
#include "match/Quality.h"
#include "match/VersionClassifier.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::match::detail {

namespace {

constexpr double kEpsilon = 1e-9;              // thresholds compare with a little slack for rounding
constexpr qsizetype kMaxGroupPostings = 20000; // a word in more records than this is no help for retrieval
constexpr int kMaxRetrievalGroups = 3;         // the 2–3 rarest tokens (note §10)
constexpr qsizetype kFuzzyExpansions = 8;
constexpr int kZoneSegmentBrackets = 100;
constexpr int kZoneSegmentParent = 200;
constexpr int kZoneSegmentHigher = 300;
constexpr double kSubsetArtist = 0.95;
constexpr double kParentTitleFactor = 0.8;     // the title found in the parent folder ("Title\01.flac")
constexpr double kArtistFoundInBrackets = 0.9; // the artist found inside brackets: a remixer (§17.5 B)
constexpr double kAlbumAgreement = 0.85;       // no remote artist, but the album agrees (§9 likely rule)
constexpr int kWeightSquashedTitle = 6;
constexpr int kWeightSquashedArtistAsTitle = 4; // reversed names: the artist's squashed key is a title
constexpr int kWeightTitleWords = 3;
constexpr int kWeightOtherWords = 2;

bool atLeast(double value, double threshold)
{
    return value >= threshold - kEpsilon;
}

std::span<const AlignToken> spanOf(const AlignTokens &tokens)
{
    return {tokens.data(), static_cast<std::size_t>(tokens.size())};
}

bool isIgnorableZoneWord(QStringView token)
{
    const lexicon::VersionWord word = lexicon::versionWord(token);
    return word != lexicon::VersionWord::None || lexicon::isFeatWord(token) || lexicon::isLightWord(token);
}

void appendZone(Query &query, AlignTokens &out, const QString &text, int segment, bool markIgnorable)
{
    QString key = Normalizer::normalize(text, FieldKind::Title);
    if (key.isEmpty()) {
        return;
    }
    query.zoneKeys.append(std::move(key));
    const qsizetype first = out.size();
    appendAlignTokens(out, query.zoneKeys.constLast(), segment);
    if (markIgnorable) {
        for (qsizetype i = first; i < out.size(); ++i) {
            out[i].ignorable = isIgnorableZoneWord(out.at(i).text);
        }
    }
}

FieldView termView(const IndexData &data, const TokenIndex &vocabulary, quint32 begin, quint16 count,
                   quint32 squashedTerm)
{
    FieldView view;
    for (quint32 k = begin; k < begin + count; ++k) {
        view.tokens.append({vocabulary.term(data.tokenPool[k]), 0, false});
    }
    const QStringView term = data.squashed.term(squashedTerm);
    view.squashed = term.size() > 2 ? term.sliced(2) : QStringView(); // the role prefix "t␟", "a␟", "l␟"
    return view;
}

DurationComparison durationFor(const Query &query, const TrackRecord &record)
{
    return query.local ? compareDurations(query.localMs, record.durationMs)
                       : compareRemoteDuration(query.remoteSec, record.durationMs);
}

// --- The token bag (note §17.5 B) --------------------------------------------------------------------------------

// Characters of the body segments holding the title that neither the title nor the artist explains (u, §17.4).
std::pair<qsizetype, qsizetype> unexplainedCharacters(const AlignTokens &body, const Alignment &title,
                                                      const Alignment &artist)
{
    QList<int> titleSegments;
    for (qsizetype j = 0; j < body.size() && j < title.scoreB.size(); ++j) {
        if (title.scoreB.at(j) > 0.0 && !titleSegments.contains(body.at(j).segment)) {
            titleSegments.append(body.at(j).segment);
        }
    }
    qsizetype total = 0;
    qsizetype unexplained = 0;
    for (qsizetype j = 0; j < body.size(); ++j) {
        const AlignToken &token = body.at(j);
        if (token.ignorable || !titleSegments.contains(token.segment)) {
            continue;
        }
        const qsizetype length = TokenRules::length(token.text);
        total += length;
        const bool byTitle = j < title.scoreB.size() && title.scoreB.at(j) > 0.0;
        const bool byArtist = j < artist.scoreB.size() && artist.scoreB.at(j) > 0.0;
        if (!byTitle && !byArtist) {
            unexplained += length;
        }
    }
    return {unexplained, total};
}

// The token bag as the matcher uses it, with the record's tags as the reference.
struct Bag
{
    ZoneBag zones;
    double title = 0.0;  // coverage, halved when another title contains ours
    double artist = 0.0; // coverage, 0 when the artist is only a remixer
    bool exact = false;  // title and artist fully covered by exact tokens, nothing unexplained
};

Bag tokenBag(const Query &query, const RecordFields &record, FieldMatchOptions options)
{
    Bag bag;
    bag.zones = zoneBag(query, record.title.tokens, record.artist.tokens, options);
    const ZoneBag &zones = bag.zones;
    bag.title = zones.unexplained > kMaxUnexplained ? zones.coverageTitle * 0.5 : zones.coverageTitle;
    bag.artist = zones.artistOnlyInBrackets ? 0.0 : zones.coverageArtist;
    bag.exact = zones.coverageTitle >= 1.0 && zones.coverageArtist >= 1.0 && !zones.fuzzy && zones.fullyExplained;
    return bag;
}

// --- Evidence and decisions (note §9) ------------------------------------------------------------------------------

// Artist sets: "A & B" and "B & A" are equal; one set inside the other (featured artists counting as optional
// members) is 0.95.
bool isSubsetArtist(const QStringList &a, const QStringList &b, const QStringList &featuredA,
                    const QStringList &featuredB)
{
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    const auto within = [](const QStringList &members, const QStringList &set, const QStringList &extra) {
        return std::ranges::all_of(members,
                                   [&set, &extra](const QString &m) { return set.contains(m) || extra.contains(m); });
    };
    return within(a, b, featuredB) || within(b, a, featuredA);
}

struct ArtistEvidence
{
    std::optional<double> score;
    bool exact = false;
    bool fuzzy = false;
};

ArtistEvidence artistEvidence(const Query &query, const FieldView &recordArtist, const RecordMeta &meta,
                              FieldMatchOptions options)
{
    ArtistEvidence evidence;
    if (!query.hasArtist || recordArtist.isEmpty()) {
        return evidence;
    }
    if (!query.artistSet.isEmpty() && query.artistSet == meta.artistSet) {
        evidence.score = 1.0;
        evidence.exact = true;
        return evidence;
    }
    const FieldMatch field = compareFields(query.artist, recordArtist, options);
    if (field.exact) {
        evidence.score = 1.0;
        evidence.exact = true;
        return evidence;
    }
    if (isSubsetArtist(query.artistSet, meta.artistSet, query.featured, meta.featured)) {
        evidence.score = std::max(field.similarity, kSubsetArtist);
        return evidence;
    }
    evidence.score = field.similarity;
    evidence.fuzzy = field.fuzzy;
    return evidence;
}

// The better of the parsed fields (A) and the token bag (B) for title and artist (note §17.5).
struct Evidence
{
    double title = 0.0;
    std::optional<double> artist;
    std::optional<double> album;
    std::optional<double> duration;
    bool fuzzy = false;
    bool keysEqual = false; // title and artist keys equal (A), or a fully explained bag
};

Evidence gatherEvidence(const Query &query, const RecordFields &fields, const RecordMeta &meta,
                        const DurationComparison &duration, const Bag &bag, FieldMatchOptions options)
{
    Evidence evidence;
    const FieldMatch title = compareFields(query.title, fields.title, options);
    const ArtistEvidence artist = artistEvidence(query, fields.artist, meta, options);
    if (!query.album.isEmpty() && !fields.album.isEmpty()) {
        evidence.album = compareFields(query.album, fields.album, options).similarity;
    }
    if (duration.known) {
        evidence.duration = duration.score;
    }
    const bool titleFromBag = bag.title > title.similarity;
    evidence.title = std::max(title.similarity, bag.title);
    evidence.artist = artist.score;
    const bool artistFromBag = bag.artist > evidence.artist.value_or(0.0) && !bag.zones.artistOnlyInBrackets;
    if (artistFromBag) {
        evidence.artist = bag.artist;
    } else if (!evidence.artist && bag.zones.artistOnlyInBrackets) {
        evidence.artist = 0.0; // the artist appears only as a remixer: contradicting evidence, not missing evidence
    }
    evidence.fuzzy = (titleFromBag ? bag.zones.fuzzy : title.fuzzy) || (artistFromBag ? bag.zones.fuzzy : artist.fuzzy);
    const bool artistExact = artist.exact || (artistFromBag && bag.exact);
    evidence.keysEqual = (title.exact && artistExact) || (bag.exact && !query.wholeNameOnly);
    return evidence;
}

FieldScores combine(const Evidence &evidence, VersionCompatibility version, const DurationComparison &duration,
                    bool wholeNameOnly)
{
    FieldScores scores;
    scores.title = evidence.title;
    scores.artist = evidence.artist;
    scores.album = evidence.album;
    scores.duration = evidence.duration;
    double weighted = kWeightTitle * evidence.title;
    double weights = kWeightTitle;
    const auto add = [&weighted, &weights](std::optional<double> value, double weight) {
        if (value) {
            weighted += weight * *value;
            weights += weight;
        }
    };
    add(evidence.artist, kWeightArtist);
    add(evidence.duration, kWeightDuration);
    add(evidence.album, kWeightAlbum);
    scores.base = weighted / weights;
    if (version == VersionCompatibility::OtherVersion || duration.vetoed()) {
        scores.multiplier = 0.0;
    }
    if (evidence.artist && atLeast(evidence.title, kContradictionMinTitle) &&
        *evidence.artist < kContradictionMaxArtist) {
        scores.multiplier *= kContradictionFactor; // same title, another artist: covers and namesakes
    }
    if (duration.beyondFar()) {
        scores.multiplier *= kFarLengthFactor;
    }
    if (wholeNameOnly) {
        scores.multiplier *= kWholeNameFactor;
    }
    scores.score = scores.base * scores.multiplier;
    return scores;
}

struct Decision
{
    Tier tier = Tier::None;
    QString text;
};

// Exact and likely before the caps (note §9 tier table).
Decision rawTier(const Evidence &evidence, const FieldScores &scores, VersionCompatibility version,
                 const DurationComparison &duration, const Query &query, bool extraEqual)
{
    const bool versionAllows = version == VersionCompatibility::Same ||
                               (version == VersionCompatibility::ProbablySame && duration.withinExact());
    const bool lengthAllows = duration.withinExact() || (!duration.known && version == VersionCompatibility::Same &&
                                                         query.keys.title.version.isExplicit());
    const bool clean = !evidence.fuzzy && extraEqual && versionAllows;
    if (clean && evidence.keysEqual && lengthAllows) {
        return {Tier::Exact, u"exact (equal keys)"_s};
    }
    if (clean && atLeast(scores.score, kExactMinScore) && atLeast(evidence.title, kExactMinTitle) && evidence.artist &&
        atLeast(*evidence.artist, kExactMinArtist) && duration.known && duration.score >= 1.0) {
        return {Tier::Exact, u"exact (score)"_s};
    }
    const bool artistAllows = evidence.artist ? atLeast(*evidence.artist, kLikelyMinArtist)
                                              : (evidence.album && atLeast(*evidence.album, kAlbumAgreement));
    if (atLeast(scores.score, kLikelyMinScore) && atLeast(evidence.title, kLikelyMinTitle) && artistAllows &&
        version != VersionCompatibility::OtherVersion) {
        return {Tier::Likely, evidence.fuzzy ? u"likely (a typo never gives exact)"_s : u"likely"_s};
    }
    return {Tier::None, u"none"_s};
}

// Caps (note §5, §9, §17.5): an unsure version, unspecified against original beyond 3 s, an ambiguous parse.
Decision applyCaps(const Decision &decision, VersionCompatibility version, const DurationComparison &duration,
                   const Query &query)
{
    if (decision.tier != Tier::None && version == VersionCompatibility::Unsure) {
        return duration.withinLikely() ? Decision{Tier::Likely, u"likely (version unsure, lengths agree)"_s}
                                       : Decision{Tier::None, u"none (version unsure, lengths don't confirm)"_s};
    }
    if (decision.tier == Tier::Exact && version == VersionCompatibility::ProbablySame && !duration.withinExact()) {
        return {Tier::Likely, u"likely (unspecified against original)"_s};
    }
    if (decision.tier == Tier::Exact && query.orderAmbiguous) {
        return {Tier::Likely, u"likely (artist/title order ambiguous)"_s};
    }
    return decision;
}

Relation decideRelation(const Evidence &evidence, Tier tier, VersionCompatibility version,
                        const DurationComparison &duration, const Query &query, const TrackRecord &record)
{
    if (tier != Tier::None) {
        const int owned = record.quality;
        const bool upgrade = query.qualityEstablished && owned != kQualityUnknown && query.quality > owned;
        return upgrade ? Relation::Upgrade : Relation::Same;
    }
    const bool sameWork =
        atLeast(evidence.title, kLikelyMinTitle) && (!evidence.artist || atLeast(*evidence.artist, kLikelyMinArtist));
    const bool versionOrLength = version == VersionCompatibility::OtherVersion ||
                                 version == VersionCompatibility::Unsure ||
                                 (duration.known && !duration.withinLikely());
    return sameWork && versionOrLength ? Relation::OtherVersion : Relation::None;
}

// --- Explanations ----------------------------------------------------------------------------------------------------

QString number(double value)
{
    return QString::number(value, 'f', 2);
}

QString joined(const AlignTokens &tokens)
{
    QStringList words;
    for (const AlignToken &token : tokens) {
        words.append(token.text.toString());
    }
    return words.join(u' ');
}

void explainScore(Scored &scored, const IndexData &data, const Query &query, const DurationComparison &duration,
                  const Bag &bag, const QString &decision)
{
    const TrackRecord &record = data.records[scored.record];
    const RecordFields fields = recordFields(data, scored.record);
    const QString version = data.metas[record.meta].version.key();
    scored.explanation.append(u"record %1 (%2): '%3' by '%4', version %5"_s.arg(record.id).arg(
        sourceName(record.source), joined(fields.title.tokens), joined(fields.artist.tokens),
        version.isEmpty() ? u"unspecified"_s : version));
    QString length = u"—"_s;
    if (duration.known) {
        length = u"%1 (Δ %2 s, exact ≤ %3, likely ≤ %4, far ≤ %5)"_s.arg(number(duration.score))
                     .arg(duration.delta)
                     .arg(duration.exactWindow)
                     .arg(duration.likelyWindow, 0, 'f', 1)
                     .arg(duration.farWindow, 0, 'f', 1);
    }
    const FieldScores &s = scored.scores;
    scored.explanation.append(u"  title %1, artist %2, length %3, album %4"_s.arg(
        number(s.title), s.artist ? number(*s.artist) : u"—"_s, length, s.album ? number(*s.album) : u"—"_s));
    if (query.hasZones) {
        scored.explanation.append(u"  token bag: title %1, artist %2, unexplained %3%4"_s.arg(
            number(bag.zones.coverageTitle), number(bag.zones.coverageArtist), number(bag.zones.unexplained),
            bag.zones.artistOnlyInBrackets ? u", artist only inside brackets (a remixer)"_s : QString()));
    }
    scored.explanation.append(u"  version %1; base %2 × %3 = %4 → %5"_s.arg(
        compatibilityName(scored.version), number(s.base), number(s.multiplier), number(s.score), decision));
}

// --- Retrieval (note §10) --------------------------------------------------------------------------------------------

using Hits = std::vector<std::pair<quint32, int>>;

void addPostings(Hits &hits, std::span<const quint32> postings, int weight)
{
    const std::size_t take = std::min<std::size_t>(postings.size(), kMaxGroupPostings);
    for (std::size_t i = 0; i < take; ++i) {
        hits.emplace_back(postings[i], weight);
    }
}

std::span<const quint32> squashedPostings(const IndexData &data, QChar role, QStringView squashed)
{
    if (squashed.isEmpty()) {
        return {};
    }
    return data.squashed.postings(data.squashed.find(QString(QString(role) + u'␟' + squashed)));
}

// One query word: its exact term, or the vocabulary terms within its typo budget.
struct WordGroup
{
    QList<quint32> terms;
    qsizetype frequency = 0;
    int weight = kWeightOtherWords;
};

void collectGroups(const IndexData &data, const AlignTokens &tokens, int weight, QStringList &seen,
                   QList<WordGroup> &groups)
{
    for (const AlignToken &token : tokens) {
        if (token.ignorable || lexicon::isLightWord(token.text) || seen.contains(token.text)) {
            continue;
        }
        seen.append(token.text.toString());
        WordGroup group;
        group.weight = weight;
        if (const quint32 term = data.words.find(token.text); term != TokenIndex::kNoTerm) {
            group.terms.append(term);
        } else {
            const QList<TokenIndex::FuzzyHit> fuzzy = data.words.fuzzyLookup(token.text, kFuzzyExpansions);
            for (const TokenIndex::FuzzyHit &hit : fuzzy) {
                group.terms.append(hit.term);
            }
        }
        for (const quint32 term : std::as_const(group.terms)) {
            group.frequency += static_cast<qsizetype>(data.words.postings(term).size());
        }
        if (group.frequency > 0) {
            groups.append(std::move(group));
        }
    }
}

QList<quint32> bestHits(const IndexData &data, Hits &hits, qsizetype limit)
{
    std::ranges::sort(hits);
    Hits merged;
    for (const auto &[record, weight] : hits) {
        if (!merged.empty() && merged.back().first == record) {
            merged.back().second += weight;
        } else {
            merged.emplace_back(record, weight);
        }
    }
    std::ranges::stable_sort(merged, [](const auto &a, const auto &b) { return a.second > b.second; });
    QList<quint32> candidates;
    for (const auto &[record, weight] : merged) {
        if (candidates.size() >= limit) {
            break;
        }
        if (!data.records[record].mix) {
            candidates.append(record);
        }
    }
    return candidates;
}

} // namespace

ZoneBag zoneBag(const Query &query, const AlignTokens &title, const AlignTokens &artist, FieldMatchOptions options)
{
    ZoneBag bag;
    if (!query.hasZones || title.isEmpty()) {
        return bag;
    }
    const Alignment titleInBody = align(spanOf(title), spanOf(query.body), options);
    bag.coverageTitle = titleInBody.coverageA();
    bag.fuzzy = titleInBody.fuzzy;
    if (bag.coverageTitle < 1.0 && !query.parent.isEmpty()) {
        const Alignment inParent = align(spanOf(title), spanOf(query.parent), options);
        bag.coverageTitle = std::max(bag.coverageTitle, kParentTitleFactor * inParent.coverageA());
    }
    Alignment artistInFolders;
    if (!artist.isEmpty()) {
        artistInFolders = align(spanOf(artist), spanOf(query.folders), options);
        bag.coverageArtist = artistInFolders.coverageA();
        bag.fuzzy = bag.fuzzy || artistInFolders.fuzzy;
        if (bag.coverageArtist < 0.5 && !query.brackets.isEmpty()) {
            const Alignment inBrackets = align(spanOf(artist), spanOf(query.brackets), options);
            bag.artistOnlyInBrackets = atLeast(inBrackets.coverageA(), kArtistFoundInBrackets);
        }
    }
    const auto [unexplained, total] = unexplainedCharacters(query.body, titleInBody, artistInFolders);
    bag.unexplained = total > 0 ? static_cast<double>(unexplained) / static_cast<double>(total) : 0.0;
    bag.fullyExplained = unexplained == 0;
    return bag;
}

Query prepareRemote(const ParsedRemote &parsed, const RemoteFile &file)
{
    Query query;
    query.keys = parsed.keys;
    query.title = fieldView(query.keys.title.field);
    query.artist = fieldView(query.keys.artist.field);
    query.album = fieldView(query.keys.album);
    query.artistSet = query.keys.artist.set;
    query.featured = query.keys.artist.featured + query.keys.title.featured;
    query.hasArtist = !query.keys.artist.field.key.isEmpty();
    query.remoteSec = file.attributes.durationSec;
    query.wholeNameOnly = parsed.wholeNameOnly;
    query.orderAmbiguous = parsed.orderAmbiguous;
    const RemoteQuality quality = assessRemoteQuality(parsed.extension, file.size, file.attributes);
    query.quality = quality.rank;
    query.qualityEstablished = quality.established;

    for (qsizetype i = 0; i < parsed.zones.segments.size(); ++i) {
        appendZone(query, query.body, parsed.zones.segments.at(i), static_cast<int>(i), true);
    }
    for (qsizetype i = 0; i < parsed.zones.brackets.size(); ++i) {
        appendZone(query, query.brackets, parsed.zones.brackets.at(i), kZoneSegmentBrackets + static_cast<int>(i),
                   false);
    }
    appendZone(query, query.parent, parsed.zones.parent, kZoneSegmentParent, false);
    query.folders = query.body;
    query.folders.append(query.parent.constData(), query.parent.size());
    for (qsizetype i = 0; i < parsed.zones.higher.size(); ++i) {
        appendZone(query, query.folders, parsed.zones.higher.at(i), kZoneSegmentHigher + static_cast<int>(i), false);
    }
    query.hasZones = !query.body.isEmpty() || !query.parent.isEmpty();
    return query;
}

Query prepareLocal(const LocalTrackQuery &local)
{
    Query query;
    query.keys = Normalizer::trackKeys(local.artist, local.title, local.album);
    query.title = fieldView(query.keys.title.field);
    query.artist = fieldView(query.keys.artist.field);
    query.album = fieldView(query.keys.album);
    query.artistSet = query.keys.artist.set;
    query.featured = query.keys.artist.featured + query.keys.title.featured;
    query.hasArtist = !query.keys.artist.field.key.isEmpty();
    query.local = true;
    query.localMs = local.durationMs;
    query.quality = qualityRank(local.quality);
    query.qualityEstablished = query.quality != kQualityUnknown;
    return query;
}

RecordFields recordFields(const IndexData &data, quint32 record)
{
    const TrackRecord &r = data.records[record];
    RecordFields fields;
    fields.title = termView(data, data.words, r.titleBegin, r.titleCount, r.titleSquashed);
    fields.artist = termView(data, data.words, r.artistBegin, r.artistCount, r.artistSquashed);
    fields.album = termView(data, data.albumWords, r.albumBegin, r.albumCount, r.albumSquashed);
    if (const auto it = data.titleLeet.constFind(record); it != data.titleLeet.cend()) {
        fields.title.leet = it.value();
    }
    if (const auto it = data.artistLeet.constFind(record); it != data.artistLeet.cend()) {
        fields.artist.leet = it.value();
    }
    return fields;
}

QList<quint32> retrieve(const IndexData &data, const Query &query, qsizetype limit)
{
    Hits hits;
    addPostings(hits, squashedPostings(data, u't', query.title.squashed), kWeightSquashedTitle);
    if (query.hasArtist) {
        addPostings(hits, squashedPostings(data, u't', query.artist.squashed), kWeightSquashedArtistAsTitle);
    }
    QList<WordGroup> groups;
    QStringList seen;
    collectGroups(data, query.title.tokens, kWeightTitleWords, seen, groups);
    collectGroups(data, query.artist.tokens, kWeightOtherWords, seen, groups);
    collectGroups(data, query.body, kWeightOtherWords, seen, groups);
    std::ranges::stable_sort(groups, {}, &WordGroup::frequency);
    int used = 0;
    for (const WordGroup &group : std::as_const(groups)) {
        if (used >= kMaxRetrievalGroups || (group.frequency > kMaxGroupPostings && used > 0)) {
            break;
        }
        for (const quint32 term : group.terms) {
            addPostings(hits, data.words.postings(term), group.weight);
        }
        ++used;
    }
    return bestHits(data, hits, limit);
}

Scored score(const IndexData &data, const Query &query, quint32 recordIndex, bool explain)
{
    const TrackRecord &record = data.records[recordIndex];
    const RecordMeta &meta = data.metas[record.meta];
    const RecordFields fields = recordFields(data, recordIndex);
    const DurationComparison duration = durationFor(query, record);
    const FieldMatchOptions options{duration.withinExact()};

    const Bag bag = tokenBag(query, fields, options);
    const Evidence evidence = gatherEvidence(query, fields, meta, duration, bag, options);
    Scored scored;
    scored.record = recordIndex;
    scored.fuzzy = evidence.fuzzy;
    scored.version = VersionClassifier::compare(query.keys.title.version, meta.version);
    scored.scores = combine(evidence, scored.version, duration, query.wholeNameOnly);
    const bool extraEqual = query.keys.title.extraKey == meta.extraKey;
    Decision decision = applyCaps(rawTier(evidence, scored.scores, scored.version, duration, query, extraEqual),
                                  scored.version, duration, query);
    scored.tier = decision.tier;
    scored.relation = decideRelation(evidence, scored.tier, scored.version, duration, query, record);
    if (scored.relation == Relation::OtherVersion) {
        decision.text += u", another version"_s;
    }
    if (explain) {
        explainScore(scored, data, query, duration, bag, decision.text);
    }
    return scored;
}

bool better(const Scored &a, const Scored &b, const IndexData &data)
{
    if (a.tier != b.tier) {
        return a.tier > b.tier;
    }
    if (std::abs(a.scores.score - b.scores.score) > kEpsilon) {
        return a.scores.score > b.scores.score;
    }
    const Source sa = data.records[a.record].source;
    const Source sb = data.records[b.record].source;
    if (sa != sb) {
        return sa < sb;
    }
    return a.record < b.record;
}

RecordMatch toRecordMatch(const IndexData &data, const Scored &scored)
{
    const TrackRecord &record = data.records[scored.record];
    RecordMatch match;
    match.id = record.id;
    match.source = record.source;
    match.albumId = record.albumId;
    match.tier = scored.tier;
    match.relation = scored.relation;
    match.version = scored.version;
    match.scores = scored.scores;
    match.explanation = scored.explanation;
    return match;
}

} // namespace selecta::match::detail
