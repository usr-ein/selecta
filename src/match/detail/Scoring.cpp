// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/Scoring.h"

#include "match/Lexicon.h"
#include "match/Quality.h"
#include "match/VersionClassifier.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::match::detail {

namespace {

constexpr double kEpsilon = 1e-9;              // thresholds compare with a little slack for rounding
constexpr qsizetype kMaxGroupPostings = 20000; // postings considered per word group
constexpr qsizetype kSeedTarget = 32;          // a third seed group joins while there are fewer candidates
constexpr qsizetype kNarrowAbove = 64;         // larger candidate lists are narrowed by title and artist words
constexpr int kMinSeedGroups = 2;              // the 2–3 rarest groups (note §10): two, so one stray word can't
constexpr int kMaxSeedGroups = 3;              // keep the true record out
constexpr qsizetype kFuzzyExpansions = 8;
constexpr int kZoneSegmentBrackets = 100;
constexpr int kZoneSegmentParent = 200;
constexpr int kZoneSegmentHigher = 300;
constexpr double kSubsetArtist = 0.95;
constexpr double kParentTitleFactor = 0.8; // the title found in the parent folder ("Title\01.flac")
static_assert(kParentTitleFactor < kLikelyMinTitle, "score() prunes without looking at the parent folder");
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

FieldView termView(const IndexData &data, const TokenIndex &vocabulary, const std::vector<TokenFacts> &facts,
                   quint32 begin, quint16 count, quint32 squashedTerm)
{
    FieldView view;
    for (quint32 k = begin; k < begin + count; ++k) {
        const quint32 term = data.tokenPool[k];
        view.tokens.append({vocabulary.term(term), 0, false, facts[term]});
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
    QVarLengthArray<int, 8> titleSegments;
    for (qsizetype j = 0; j < body.size() && j < title.scoreB.size(); ++j) {
        if (title.scoreB[j] > 0.0 && !titleSegments.contains(body.at(j).segment)) {
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
        const qsizetype length = token.facts.length;
        total += length;
        const bool byTitle = j < title.scoreB.size() && title.scoreB[j] > 0.0;
        const bool byArtist = j < artist.scoreB.size() && artist.scoreB[j] > 0.0;
        if (!byTitle && !byArtist) {
            unexplained += length;
        }
    }
    return {unexplained, total};
}

// The title half of the token bag: computed first, because its coverage bounds the bag's title score.
struct ZoneTitle
{
    Alignment inBody;
    double coverage = 0.0;
    bool fuzzy = false;
};

ZoneTitle zoneTitle(const Query &query, const AlignTokens &title, FieldMatchOptions options)
{
    ZoneTitle zone;
    if (!query.hasZones || title.isEmpty()) {
        return zone;
    }
    zone.inBody = align(spanOf(title), spanOf(query.body), options);
    zone.coverage = zone.inBody.coverageA();
    zone.fuzzy = zone.inBody.fuzzy;
    if (zone.coverage < 1.0 && !query.parent.isEmpty()) {
        const Alignment inParent = align(spanOf(title), spanOf(query.parent), options);
        zone.coverage = std::max(zone.coverage, kParentTitleFactor * inParent.coverageA());
    }
    return zone;
}

ZoneBag completeBag(const Query &query, const ZoneTitle &title, const AlignTokens &artist, FieldMatchOptions options)
{
    ZoneBag bag;
    bag.coverageTitle = title.coverage;
    bag.fuzzy = title.fuzzy;
    if (!query.hasZones) {
        return bag;
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
    const auto [unexplained, total] = unexplainedCharacters(query.body, title.inBody, artistInFolders);
    bag.unexplained = total > 0 ? static_cast<double>(unexplained) / static_cast<double>(total) : 0.0;
    bag.fullyExplained = unexplained == 0;
    return bag;
}

// The token bag as the matcher uses it, with the record's tags as the reference.
struct Bag
{
    ZoneBag zones;
    double title = 0.0;  // coverage, halved when another title contains ours
    double artist = 0.0; // coverage, 0 when the artist is only a remixer
    bool exact = false;  // title and artist fully covered by exact tokens, nothing unexplained
};

Bag tokenBag(const Query &query, const ZoneTitle &title, const RecordFields &record, FieldMatchOptions options)
{
    Bag bag;
    bag.zones = completeBag(query, title, record.artist.tokens, options);
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
    const FieldMatch field = compareFields(query.artist, recordArtist, options, /*withPairs=*/false);
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

Evidence gatherEvidence(const Query &query, const FieldMatch &title, const RecordFields &fields, const RecordMeta &meta,
                        const DurationComparison &duration, const Bag &bag, FieldMatchOptions options)
{
    Evidence evidence;
    const ArtistEvidence artist = artistEvidence(query, fields.artist, meta, options);
    if (!query.album.isEmpty() && !fields.album.isEmpty()) {
        evidence.album = compareFields(query.album, fields.album, options, /*withPairs=*/false).similarity;
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

// One query word: its exact term, or the vocabulary terms within its typo budget; or a squashed key.
struct WordGroup
{
    std::span<const quint32> postings; // one term: its postings
    std::vector<quint32> merged;       // several terms: the union of their postings
    qsizetype frequency = 0;           // Σ postings of the terms
    int weight = kWeightOtherWords;
    bool seeds = true;   // a title or artist word, or the title's squashed key: every true candidate has one
    bool narrows = true; // likewise; body-only words and the reversed-name key only boost

    // The records holding the word: sorted, unique.
    std::span<const quint32> records() const { return merged.empty() ? postings : std::span<const quint32>(merged); }
};

using WordGroups = QVarLengthArray<WordGroup, 16>;

void addGroup(const TokenIndex &vocabulary, std::span<const quint32> terms, WordGroup group, WordGroups &groups)
{
    if (terms.size() == 1) {
        group.postings = vocabulary.postings(terms.front());
        group.frequency = static_cast<qsizetype>(group.postings.size());
    } else {
        std::vector<quint32> united; // postings are sorted and unique: linear unions, no sort
        for (const quint32 term : terms) {
            const std::span<const quint32> postings = vocabulary.postings(term);
            group.frequency += static_cast<qsizetype>(postings.size());
            united.clear();
            united.reserve(group.merged.size() + postings.size());
            std::ranges::set_union(group.merged, postings, std::back_inserter(united));
            group.merged.swap(united);
        }
    }
    if (group.frequency > 0) {
        groups.append(std::move(group));
    }
}

void addSquashedGroup(const IndexData &data, QChar role, QStringView squashed, int weight, bool seeds,
                      WordGroups &groups)
{
    if (squashed.isEmpty()) {
        return;
    }
    QVarLengthArray<char16_t, 64> key; // "t␟" + key, the squashed index's role prefix
    key.append(role.unicode());
    key.append(u'␟');
    key.append(squashed.utf16(), squashed.size());
    const quint32 term = data.squashed.find(QStringView(key.constData(), key.size()));
    if (term == TokenIndex::kNoTerm) {
        return;
    }
    WordGroup group;
    group.weight = weight;
    group.seeds = seeds;
    group.narrows = seeds;
    addGroup(data.squashed, std::span(&term, 1), std::move(group), groups);
}

void collectGroups(const IndexData &data, const AlignTokens &tokens, int weight, bool narrows,
                   QVarLengthArray<QStringView, 16> &seen, WordGroups &groups)
{
    for (const AlignToken &token : tokens) {
        if (token.ignorable || token.facts.light || seen.contains(token.text)) {
            continue;
        }
        seen.append(token.text);
        WordGroup group;
        group.weight = weight;
        group.seeds = narrows;
        group.narrows = narrows;
        if (const quint32 term = data.words.find(token.text); term != TokenIndex::kNoTerm) {
            addGroup(data.words, std::span(&term, 1), std::move(group), groups);
            continue;
        }
        const QList<TokenIndex::FuzzyHit> fuzzy = data.words.fuzzyLookup(token.text, kFuzzyExpansions);
        QVarLengthArray<quint32, kFuzzyExpansions> terms;
        for (const TokenIndex::FuzzyHit &hit : fuzzy) {
            terms.append(hit.term);
        }
        addGroup(data.words, std::span(terms.constData(), static_cast<std::size_t>(terms.size())), std::move(group),
                 groups);
    }
}

struct Candidate
{
    quint32 record = 0;
    int weight = 0;
};

// Unites a group's records into the sorted candidate list (weights added for records already present).
void unite(std::vector<Candidate> &candidates, const WordGroup &group)
{
    const std::span<const quint32> all = group.records();
    const std::span<const quint32> records = all.first(std::min<std::size_t>(all.size(), kMaxGroupPostings));
    std::vector<Candidate> merged;
    merged.reserve(candidates.size() + records.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < candidates.size() || j < records.size()) {
        if (j == records.size() || (i < candidates.size() && candidates[i].record < records[j])) {
            merged.push_back(candidates[i++]);
        } else if (i == candidates.size() || records[j] < candidates[i].record) {
            merged.push_back({records[j++], group.weight});
        } else {
            merged.push_back({candidates[i].record, candidates[i].weight + group.weight});
            ++i;
            ++j;
        }
    }
    candidates = std::move(merged);
}

// Narrows a large candidate list to the records in the group (unless none are), or boosts the ones that are.
// Both lists are sorted: a merge when their sizes are comparable, binary searches when the group is much larger.
void narrowOrBoost(std::vector<Candidate> &candidates, const WordGroup &group)
{
    constexpr std::size_t kSearchAbove = 16; // the group this many times larger than the candidate list
    const std::span<const quint32> records = group.records();
    std::vector<Candidate> inGroup;
    const auto boost = [&inGroup, &group](Candidate &candidate) {
        candidate.weight += group.weight;
        inGroup.push_back(candidate);
    };
    if (records.size() > kSearchAbove * candidates.size()) {
        for (Candidate &candidate : candidates) {
            if (std::ranges::binary_search(records, candidate.record)) {
                boost(candidate);
            }
        }
    } else {
        std::size_t j = 0;
        for (Candidate &candidate : candidates) {
            while (j < records.size() && records[j] < candidate.record) {
                ++j;
            }
            if (j < records.size() && records[j] == candidate.record) {
                boost(candidate);
            }
        }
    }
    if (group.narrows && static_cast<qsizetype>(candidates.size()) > kNarrowAbove && !inGroup.empty()) {
        candidates = std::move(inGroup); // the word may be missing from a record; never narrow to nothing
    }
}

} // namespace

ZoneBag zoneBag(const Query &query, const AlignTokens &title, const AlignTokens &artist, FieldMatchOptions options)
{
    return completeBag(query, zoneTitle(query, title, options), artist, options);
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

FieldView recordTitle(const IndexData &data, quint32 record)
{
    const TrackRecord &r = data.records[record];
    FieldView view = termView(data, data.words, data.wordFacts, r.titleBegin, r.titleCount, r.titleSquashed);
    if (r.titleLeet) {
        view.leet = *data.titleLeet.constFind(record);
    }
    return view;
}

FieldView recordArtist(const IndexData &data, quint32 record)
{
    const TrackRecord &r = data.records[record];
    FieldView view = termView(data, data.words, data.wordFacts, r.artistBegin, r.artistCount, r.artistSquashed);
    if (r.artistLeet) {
        view.leet = *data.artistLeet.constFind(record);
    }
    return view;
}

FieldView recordAlbum(const IndexData &data, quint32 record)
{
    const TrackRecord &r = data.records[record];
    return termView(data, data.albumWords, data.albumWordFacts, r.albumBegin, r.albumCount, r.albumSquashed);
}

RecordFields recordFields(const IndexData &data, quint32 record)
{
    return {recordTitle(data, record), recordArtist(data, record), recordAlbum(data, record)};
}

QList<quint32> retrieve(const IndexData &data, const Query &query, qsizetype limit)
{
    WordGroups groups;
    addSquashedGroup(data, u't', query.title.squashed, kWeightSquashedTitle, true, groups);
    if (query.hasArtist) {
        // Reversed names ("Glue - Bicep" parsed with "Glue" as the artist): only a booster, never a seed.
        addSquashedGroup(data, u't', query.artist.squashed, kWeightSquashedArtistAsTitle, false, groups);
    }
    QVarLengthArray<QStringView, 16> seen;
    collectGroups(data, query.title.tokens, kWeightTitleWords, true, seen, groups);
    collectGroups(data, query.artist.tokens, kWeightOtherWords, true, seen, groups);
    collectGroups(data, query.body, kWeightOtherWords, false, seen, groups);
    std::ranges::stable_sort(groups, {}, &WordGroup::frequency);

    // The rarest seed groups are united (two, and a third while candidates are few); then every other group
    // narrows a large list or boosts the candidates it contains.
    std::vector<Candidate> candidates;
    QVarLengthArray<bool, 16> used(groups.size(), false);
    int seeded = 0;
    for (qsizetype g = 0; g < groups.size(); ++g) {
        const bool more = seeded < kMinSeedGroups ||
                          (seeded < kMaxSeedGroups && static_cast<qsizetype>(candidates.size()) < kSeedTarget);
        if (!more) {
            break;
        }
        if (groups.at(g).seeds) {
            unite(candidates, groups.at(g));
            used[g] = true;
            ++seeded;
        }
    }
    for (qsizetype g = 0; g < groups.size(); ++g) {
        if (!used[g]) {
            narrowOrBoost(candidates, groups.at(g));
        }
    }
    // The `limit` heaviest records that aren't mixes; equal weights in record order.
    std::erase_if(candidates, [&data](Candidate c) { return data.records[c.record].mix; });
    const auto heavier = [](Candidate a, Candidate b) {
        return a.weight != b.weight ? a.weight > b.weight : a.record < b.record;
    };
    const auto keep =
        static_cast<std::ptrdiff_t>(std::min<std::size_t>(candidates.size(), std::max<qsizetype>(limit, 0)));
    std::ranges::partial_sort(candidates, candidates.begin() + keep, heavier);
    QList<quint32> records;
    records.reserve(keep);
    for (std::ptrdiff_t k = 0; k < keep; ++k) {
        records.append(candidates[static_cast<std::size_t>(k)].record);
    }
    return records;
}

Scored score(const IndexData &data, const Query &query, quint32 recordIndex, bool explain)
{
    const TrackRecord &record = data.records[recordIndex];
    const RecordMeta &meta = data.metas[record.meta];
    const DurationComparison duration = durationFor(query, record);
    const FieldMatchOptions options{duration.withinExact()};
    Scored scored;
    scored.record = recordIndex;

    // Every positive tier and the "other version" relation need a title of 0.85 or more, on the parsed fields or in
    // the token bag (whose title score is at most its coverage; the parent-folder route stays below 0.85): below that
    // on both, nothing else can matter. Cheap upper bounds reject most candidates before any alignment.
    RecordFields fields;
    fields.title = recordTitle(data, recordIndex);
    const bool titleMayReach =
        similarityMayReach(query.title, fields.title, kLikelyMinTitle) ||
        (query.hasZones && coverageMayReach(spanOf(fields.title.tokens), spanOf(query.body), kLikelyMinTitle));
    if (!titleMayReach) {
        if (explain) {
            scored.explanation.append(u"record %1: title below the likely threshold on every route"_s.arg(record.id));
        }
        return scored;
    }
    const FieldMatch title = compareFields(query.title, fields.title, options, /*withPairs=*/false);
    const ZoneTitle zone = zoneTitle(query, fields.title.tokens, options);
    if (!atLeast(std::max(title.similarity, zone.coverage), kLikelyMinTitle)) {
        scored.scores.title = std::max(title.similarity, zone.coverage);
        if (explain) {
            scored.explanation.append(
                u"record %1: title %2, below the likely threshold"_s.arg(record.id).arg(number(scored.scores.title)));
        }
        return scored;
    }
    fields.artist = recordArtist(data, recordIndex);
    fields.album = recordAlbum(data, recordIndex);

    const Bag bag = tokenBag(query, zone, fields, options);
    const Evidence evidence = gatherEvidence(query, title, fields, meta, duration, bag, options);
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
    if (a.version != b.version) {
        return a.version < b.version; // Same before ProbablySame before Unsure (enum order)
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
