// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/AlbumMatcher.h"

#include "match/Duration.h"
#include "match/Normalizer.h"
#include "match/Similarity.h"
#include "match/detail/Hungarian.h"
#include "match/detail/IndexData.h"
#include "match/detail/Scoring.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace {

// Album distance (note §8 step 4, beets' weights).
constexpr double kWeightArtist = 3.0;
constexpr double kWeightTitle = 3.0;
constexpr double kWeightYear = 1.0;
constexpr double kWeightTracks = 2.0;
constexpr double kWeightMissing = 0.9; // per local track without a remote counterpart
constexpr double kWeightExtra = 0.6;   // per remote track without a local counterpart
constexpr int kFreeYears = 1;          // reissue years: ±1 costs nothing

constexpr double kTrackNumberPenalty = 0.1; // note §8 step 3
constexpr double kLengthOnlyWeight = 0.5;   // a file whose name says nothing is placed by its length, at half weight
constexpr int kSubsetTracks = 3;            // signature candidates up to 3 tracks shorter or longer (bonus tracks)
constexpr qsizetype kMaxCandidatesPerRoute = 16;
constexpr std::size_t kMaxTokenPostings = 5000; // album words more common than this don't propose candidates
constexpr double kNamesContradict = 0.5;        // artist and title both below this: the names deny a signature match

// One audio file of the folder, prepared once.
struct RemoteTrack
{
    qsizetype input = -1; // index into the files passed to match()
    std::unique_ptr<detail::Query> query;
    bool named = false; // a usable title
    int durationSec = 0;
    int trackNumber = 0;
    std::optional<detail::Scored> owner; // the best ownership anywhere at tier ≥ likely
};

// The folder's album as the files describe it.
struct RemoteAlbum
{
    QString artist; // the folder's artist, or the track artist most files agree on
    QString title;
    int year = 0;
    bool variousArtists = false;
    ArtistKeys artistKeys;
    FieldKey titleKey;
};

struct Vote
{
    QString value;
    qsizetype count = 0;
};

// The most frequent value by normalised key (the first spelling seen stands for its key); ties go to the first key.
Vote mostCommon(const QStringList &values, FieldKind kind)
{
    QHash<QString, qsizetype> counts;
    QHash<QString, QString> spelling;
    QStringList order;
    for (const QString &value : values) {
        const QString key = Normalizer::normalize(value, kind);
        if (key.isEmpty()) {
            continue;
        }
        if (!spelling.contains(key)) {
            spelling.insert(key, value);
            order.append(key);
        }
        ++counts[key];
    }
    Vote best;
    for (const QString &key : std::as_const(order)) {
        if (counts.value(key) > best.count) {
            best = {spelling.value(key), counts.value(key)};
        }
    }
    return best;
}

RemoteAlbum remoteAlbum(const QList<RemoteAlbumFile> &files, const std::vector<RemoteTrack> &tracks)
{
    RemoteAlbum album;
    QStringList albumArtists;
    QStringList artists;
    QStringList titles;
    QHash<int, qsizetype> years;
    for (const RemoteTrack &track : tracks) {
        const ParsedRemote &parsed = files.at(track.input).parsed;
        if (parsed.albumArtist.isSet()) {
            albumArtists.append(parsed.albumArtist.value);
        }
        if (parsed.artist.isSet()) {
            artists.append(parsed.artist.value);
        }
        if (parsed.album.isSet()) {
            titles.append(parsed.album.value);
        }
        if (parsed.year.isSet() && parsed.year.value > 0) {
            ++years[parsed.year.value];
        }
        album.variousArtists = album.variousArtists || parsed.variousArtists;
    }
    album.artist = mostCommon(albumArtists, FieldKind::Artist).value;
    if (album.artist.isEmpty()) {
        const Vote constant = mostCommon(artists, FieldKind::Artist);
        if (2 * constant.count >= static_cast<qsizetype>(tracks.size())) {
            album.artist = constant.value; // a constant track artist stands in for the folder's (note §8 step 1)
        }
    }
    album.title = mostCommon(titles, FieldKind::Title).value;
    qsizetype yearVotes = 0;
    for (auto it = years.cbegin(); it != years.cend(); ++it) {
        if (it.value() > yearVotes || (it.value() == yearVotes && it.key() < album.year)) {
            album.year = it.key();
            yearVotes = it.value();
        }
    }
    album.artistKeys = Normalizer::artistKeys(album.artist);
    album.titleKey = Normalizer::albumKey(album.title);
    return album;
}

std::optional<detail::Scored> ownerOf(const detail::IndexData &data, const detail::Query &query)
{
    const QList<quint32> candidates = detail::retrieve(data, query, kMaxCandidates);
    detail::BestMatches best = detail::bestOf(data, query, candidates, false, nullptr);
    if (best.collection && best.inbox) {
        return detail::better(*best.inbox, *best.collection, data) ? std::move(best.inbox) : std::move(best.collection);
    }
    return best.collection ? std::move(best.collection) : std::move(best.inbox);
}

// How many lengths of two sorted lists pair up within ±3 s: walking both lists greedily finds the most pairs.
qsizetype pairedLengths(const std::vector<int> &a, const std::vector<int> &b)
{
    qsizetype pairs = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::abs(a[i] - b[j]) <= static_cast<int>(kSignatureWindowSec)) {
            ++pairs;
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return pairs;
}

// Candidate albums, strongest routes first, without duplicates.
class Candidates
{
public:
    explicit Candidates(std::size_t albums)
        : m_seen(albums, false)
    {}

    void add(qint32 album)
    {
        if (album >= 0 && static_cast<std::size_t>(album) < m_seen.size() && !m_seen[static_cast<std::size_t>(album)]) {
            m_seen[static_cast<std::size_t>(album)] = true;
            m_list.push_back(album);
        }
    }

    const std::vector<qint32> &list() const { return m_list; }

private:
    std::vector<bool> m_seen;
    std::vector<qint32> m_list;
};

// Adds the albums with at least `needed` votes, most votes first (album index on ties), at most
// kMaxCandidatesPerRoute.
void addRanked(const QHash<qint32, qsizetype> &votes, qsizetype needed, Candidates &candidates)
{
    std::vector<std::pair<qsizetype, qint32>> ranked;
    ranked.reserve(static_cast<std::size_t>(votes.size()));
    for (auto it = votes.cbegin(); it != votes.cend(); ++it) {
        if (it.value() >= needed) {
            ranked.emplace_back(it.value(), it.key());
        }
    }
    std::ranges::sort(ranked, [](const auto &x, const auto &y) {
        return x.first != y.first ? x.first > y.first : x.second < y.second;
    });
    const std::size_t count = std::min(ranked.size(), static_cast<std::size_t>(kMaxCandidatesPerRoute));
    for (std::size_t k = 0; k < count; ++k) {
        candidates.add(ranked[k].second);
    }
}

// The albums of a record list, by how many of the lists name them: at least half of them.
void addVotedAlbums(const detail::IndexData &data, const std::vector<std::span<const quint32>> &lists,
                    Candidates &candidates)
{
    QHash<qint32, qsizetype> votes;
    for (const std::span<const quint32> records : lists) {
        QSet<qint32> albums;
        for (const quint32 record : records) {
            albums.insert(data.records[record].album);
        }
        for (const qint32 album : std::as_const(albums)) {
            if (album >= 0) {
                ++votes[album];
            }
        }
    }
    addRanked(votes, std::max<qsizetype>(1, (static_cast<qsizetype>(lists.size()) + 1) / 2), candidates);
}

// The albums holding the tracks owned, most owned tracks first.
void addOwnerAlbums(const detail::IndexData &data, const std::vector<RemoteTrack> &tracks, Candidates &candidates)
{
    QHash<qint32, qsizetype> owned;
    for (const RemoteTrack &track : tracks) {
        if (track.owner) {
            if (const qint32 album = data.records[track.owner->record].album; album >= 0) {
                ++owned[album];
            }
        }
    }
    addRanked(owned, 1, candidates);
}

// Albums named like the folder: the album key exactly, else most of the album-title words.
void addNamedAlbums(const detail::IndexData &data, const RemoteAlbum &remote, Candidates &candidates)
{
    if (remote.titleKey.squashed.isEmpty()) {
        return;
    }
    const quint32 term = data.squashed.find(QString(u"l␟"_s + remote.titleKey.squashed));
    if (term != TokenIndex::kNoTerm) {
        addVotedAlbums(data, {data.squashed.postings(term)}, candidates);
    }
    std::vector<std::span<const quint32>> lists;
    const QStringList words = remote.titleKey.key.split(u' ', Qt::SkipEmptyParts); // the key is normalised already
    for (const QString &word : words) {
        const std::span<const quint32> postings = data.albumWords.postings(data.albumWords.find(word));
        if (!postings.empty() && postings.size() <= kMaxTokenPostings) {
            lists.push_back(postings);
        }
    }
    if (!lists.empty()) {
        addVotedAlbums(data, lists, candidates);
    }
}

// Albums with the folder's duration signature (note §8 step 2): the same track count and shortest-length band, or a
// few tracks more or fewer, in the neighbouring bands too; ranked by lengths paired.
void addSignatureAlbums(const detail::IndexData &data, const std::vector<int> &lengths, Candidates &candidates)
{
    const auto count = static_cast<int>(lengths.size());
    if (count < kSignatureMinTracks) {
        return;
    }
    const int band = lengths.front() / detail::kSignatureBandSec;
    QHash<qint32, qsizetype> paired;
    for (int tracks = std::max(1, count - kSubsetTracks); tracks <= count + kSubsetTracks; ++tracks) {
        for (int b = std::max(0, band - 1); b <= band + 1; ++b) {
            const QList<quint32> albums = data.albumsBySignature.values(detail::signatureKey(tracks, b));
            for (const quint32 album : albums) {
                paired.insert(static_cast<qint32>(album), pairedLengths(lengths, data.albums[album].sortedDurations));
            }
        }
    }
    addRanked(paired, kSignatureMinTracks, candidates);
}

struct AlbumDistance
{
    double weighted = 0.0;
    double weights = 0.0;
    QStringList parts; // for the explanation

    void add(double weight, double distance, const QString &name)
    {
        weighted += weight * distance;
        weights += weight;
        parts.append(u"%1 %2"_s.arg(name, QString::number(distance, 'f', 2)));
    }
    double value() const { return weights > 0.0 ? weighted / weights : 1.0; }
};

std::optional<double> artistSimilarity(const RemoteAlbum &remote, const detail::AlbumRecord &album)
{
    if (remote.variousArtists && album.variousArtists) {
        return 1.0;
    }
    if (remote.artistKeys.field.key.isEmpty() || album.artist.key.isEmpty()) {
        return std::nullopt;
    }
    if (!remote.artistKeys.set.isEmpty() && remote.artistKeys.set == album.artistSet) {
        return 1.0;
    }
    return FieldMatcher::compare(remote.artistKeys.field, album.artist).similarity;
}

std::optional<double> titleSimilarity(const RemoteAlbum &remote, const detail::AlbumRecord &album)
{
    if (remote.titleKey.key.isEmpty() || album.title.key.isEmpty()) {
        return std::nullopt;
    }
    return FieldMatcher::compare(remote.titleKey, album.title).similarity;
}

// The name terms of the album distance (artist 3, title 3, year 1 with ±1 free); whether artist and title both deny
// the album.
AlbumDistance nameDistance(const RemoteAlbum &remote, const detail::AlbumRecord &album, bool &namesContradict)
{
    AlbumDistance distance;
    const std::optional<double> artist = artistSimilarity(remote, album);
    const std::optional<double> title = titleSimilarity(remote, album);
    if (artist) {
        distance.add(kWeightArtist, 1.0 - *artist, u"artist"_s);
    }
    if (title) {
        distance.add(kWeightTitle, 1.0 - *title, u"title"_s);
    }
    if (remote.year > 0 && album.year > 0) {
        distance.add(kWeightYear, std::abs(remote.year - album.year) <= kFreeYears ? 0.0 : 1.0, u"year"_s);
    }
    namesContradict = artist && title && *artist < kNamesContradict && *title < kNamesContradict;
    return distance;
}

// How well a remote file fits a local track, in [0, 1]: its §9 score when its name is usable, or half of the length
// agreement when the name says nothing (unparseable, another script) — whichever is higher.
double fit(const detail::IndexData &data, const RemoteTrack &track, quint32 record, Tier &tier)
{
    tier = Tier::None;
    double name = 0.0;
    if (track.named) {
        const detail::Scored scored = detail::score(data, *track.query, record, false);
        name = scored.scores.score;
        tier = scored.tier;
    }
    const DurationComparison length = compareRemoteDuration(track.durationSec, data.records[record].durationMs);
    return std::max(name, length.known ? kLengthOnlyWeight * length.score : 0.0);
}

// Every remote file against every track of the album: fit, tier and assignment cost (note §8 step 3).
struct FitMatrix
{
    qsizetype rows = 0;
    qsizetype cols = 0;
    std::vector<double> fits;
    std::vector<double> cost;
    std::vector<Tier> tiers;

    std::size_t at(qsizetype i, qsizetype j) const { return static_cast<std::size_t>((i * cols) + j); }
};

FitMatrix fitMatrix(const detail::IndexData &data, const detail::AlbumRecord &album,
                    const std::vector<RemoteTrack> &tracks)
{
    FitMatrix matrix;
    matrix.rows = static_cast<qsizetype>(tracks.size());
    matrix.cols = static_cast<qsizetype>(album.tracks.size());
    const auto size = static_cast<std::size_t>(matrix.rows * matrix.cols);
    matrix.fits.assign(size, 0.0);
    matrix.cost.assign(size, 1.0);
    matrix.tiers.assign(size, Tier::None);
    for (qsizetype i = 0; i < matrix.rows; ++i) {
        const RemoteTrack &track = tracks[static_cast<std::size_t>(i)];
        for (qsizetype j = 0; j < matrix.cols; ++j) {
            const quint32 record = album.tracks[static_cast<std::size_t>(j)];
            const std::size_t k = matrix.at(i, j);
            matrix.fits[k] = fit(data, track, record, matrix.tiers[k]);
            const int localNumber = data.records[record].trackNumber;
            const bool numbersDiffer = track.trackNumber > 0 && localNumber > 0 && track.trackNumber != localNumber;
            matrix.cost[k] = 1.0 - matrix.fits[k] + (numbersDiffer ? kTrackNumberPenalty : 0.0);
        }
    }
    return matrix;
}

// The assigned pairs with some evidence become the mapping; returns the sum of their distances. A signature that
// holds vouches for pairs whose lengths agree within ±3 s (T48).
double mapTracks(const detail::IndexData &data, const detail::AlbumRecord &album,
                 const std::vector<RemoteTrack> &tracks, const FitMatrix &matrix, bool signatureHolds,
                 AlbumMatch &match)
{
    double total = 0.0;
    for (const auto &[i, j] : detail::minimumCostAssignment(matrix.cost, matrix.rows, matrix.cols)) {
        const std::size_t k = matrix.at(i, j);
        if (matrix.fits[k] <= 0.0) {
            continue; // nothing places this file on this track: it stays extra, the track missing
        }
        const RemoteTrack &track = tracks[static_cast<std::size_t>(i)];
        const detail::TrackRecord &record = data.records[album.tracks[static_cast<std::size_t>(j)]];
        AlbumTrackMapping mapping;
        mapping.remoteIndex = track.input;
        mapping.trackId = record.id;
        mapping.tier = matrix.tiers[k];
        mapping.distance = std::min(1.0, matrix.cost[k]);
        if (signatureHolds && mapping.tier == Tier::None) {
            const DurationComparison length = compareRemoteDuration(track.durationSec, record.durationMs);
            if (length.known && length.delta <= kSignatureWindowSec) {
                mapping.tier = Tier::Likely;
            }
        }
        total += mapping.distance;
        match.mapping.append(mapping);
    }
    return total;
}

// In collection: distance ≤ 0.20 and ≥ 90 % of the remote tracks mapped at tier ≥ likely; exact when every remote
// track is mapped exactly. Otherwise a signature that holds makes it likely by itself.
void decide(AlbumMatch &match, qsizetype remoteTracks, bool signatureHolds)
{
    qsizetype exact = 0;
    for (const AlbumTrackMapping &mapping : std::as_const(match.mapping)) {
        match.mappedLikely += mapping.tier != Tier::None ? 1 : 0;
        exact += mapping.tier == Tier::Exact ? 1 : 0;
    }
    match.inCollection = match.distance <= kAlbumInCollectionDistance + 1e-9 &&
                         static_cast<double>(match.mappedLikely) >=
                             (kAlbumInCollectionCoverage * static_cast<double>(remoteTracks)) - 1e-9;
    if (match.inCollection) {
        match.tier = exact == remoteTracks ? Tier::Exact : Tier::Likely;
    } else if (signatureHolds) {
        match.signatureOnly = true; // the lengths say it's this release; the names don't confirm it (T48)
        match.inCollection = true;
        match.tier = Tier::Likely;
    }
    match.found = match.mappedLikely > 0 || signatureHolds;
}

struct Evaluation
{
    AlbumMatch match;
    qint32 album = -1;
    QString explanation;
};

Evaluation evaluate(const detail::IndexData &data, qint32 albumIndex, const std::vector<RemoteTrack> &tracks,
                    const RemoteAlbum &remote, const std::vector<int> &lengths)
{
    const detail::AlbumRecord &album = data.albums[static_cast<std::size_t>(albumIndex)];
    Evaluation evaluation;
    evaluation.album = albumIndex;
    AlbumMatch &match = evaluation.match;
    match.albumId = album.id;
    match.source = album.source;
    match.localTracks = static_cast<int>(album.tracks.size());
    const auto n = static_cast<qsizetype>(tracks.size());
    const auto m = static_cast<qsizetype>(album.tracks.size());

    // A full duration signature: every length known, the same count (≥ 5), every sorted length within ±3 s.
    const bool allLengths = std::cmp_equal(lengths.size(), n) && std::cmp_equal(album.sortedDurations.size(), m);
    const bool signature =
        allLengths && n == m && n >= kSignatureMinTracks && pairedLengths(lengths, album.sortedDurations) == n;
    bool namesContradict = false;
    AlbumDistance distance = nameDistance(remote, album, namesContradict);
    const bool signatureHolds = signature && !namesContradict;

    const double trackDistance = mapTracks(data, album, tracks, fitMatrix(data, album, tracks), signatureHolds, match);
    const auto mapped = static_cast<double>(match.mapping.size());
    if (!match.mapping.isEmpty()) {
        distance.add(kWeightTracks, trackDistance / mapped, u"tracks"_s);
    }
    const double missing = static_cast<double>(m) - mapped;
    const double extra = static_cast<double>(n) - mapped;
    distance.weighted += (kWeightMissing * missing) + (kWeightExtra * extra);
    distance.weights += (kWeightMissing * missing) + (kWeightExtra * extra);
    match.distance = distance.value();
    decide(match, n, signatureHolds);

    QString signatureText;
    if (signature) {
        signatureText = namesContradict ? u", length signature (names contradict it)"_s : u", length signature"_s;
    }
    QString verdict = u"not in collection"_s;
    if (match.inCollection) {
        verdict = tierName(match.tier) + (match.signatureOnly ? u" by signature"_s : QString());
    }
    evaluation.explanation =
        u"album %1: distance %2 (%3; missing %4, extra %5), %6 of %7 likely or better%8 → %9"_s.arg(album.id)
            .arg(QString::number(match.distance, 'f', 2), distance.parts.join(u", "))
            .arg(missing)
            .arg(extra)
            .arg(match.mappedLikely)
            .arg(n)
            .arg(signatureText, verdict);
    return evaluation;
}

// The better album match: in collection, tier, tracks vouched for, distance, collection before Inbox, album id.
bool betterAlbum(const AlbumMatch &a, const AlbumMatch &b)
{
    if (a.inCollection != b.inCollection) {
        return a.inCollection;
    }
    if (a.tier != b.tier) {
        return a.tier > b.tier;
    }
    if (a.mappedLikely != b.mappedLikely) {
        return a.mappedLikely > b.mappedLikely;
    }
    if (std::abs(a.distance - b.distance) > 1e-9) {
        return a.distance < b.distance;
    }
    if (a.source != b.source) {
        return a.source < b.source;
    }
    return a.albumId < b.albumId;
}

} // namespace

AlbumMatcher::AlbumMatcher(std::shared_ptr<const MatchIndex> index)
    : m_index(std::move(index))
{}

namespace {

// The folder's audio files, each prepared once and owned anywhere or not; their known lengths, sorted.
std::vector<RemoteTrack> prepareTracks(const detail::IndexData &data, const QList<RemoteAlbumFile> &files,
                                       AlbumMatchResult &result, std::vector<int> &lengths)
{
    std::vector<RemoteTrack> tracks;
    for (qsizetype i = 0; i < files.size() && std::cmp_less(tracks.size(), kMaxAlbumTracks); ++i) {
        const RemoteAlbumFile &file = files.at(i);
        if (file.parsed.kind != FileKind::Audio) {
            continue;
        }
        RemoteTrack track;
        track.input = i;
        track.query = std::make_unique<detail::Query>(detail::prepareRemote(file.parsed, file.file));
        track.named = !file.parsed.unparseable && !track.query->title.isEmpty();
        track.durationSec = std::max(0, file.file.attributes.durationSec);
        track.trackNumber = file.parsed.track.isSet() ? file.parsed.track.value : 0;
        if (track.named && !isMixLength(track.durationSec)) {
            track.owner = ownerOf(data, *track.query);
        }
        if (track.owner) {
            result.trackTiers[i] = track.owner->tier;
        }
        if (track.durationSec > 0) {
            lengths.push_back(track.durationSec);
        }
        tracks.push_back(std::move(track));
    }
    std::ranges::sort(lengths);
    result.remoteTracks = static_cast<int>(tracks.size());
    return tracks;
}

// "owned 7/12 (3 as singles)": owned anywhere at tier ≥ likely, and of those, owned outside the best album.
void countCoverage(const detail::IndexData &data, const std::vector<RemoteTrack> &tracks, qint32 bestAlbum,
                   AlbumMatchResult &result)
{
    for (const RemoteTrack &track : tracks) {
        if (!track.owner) {
            continue;
        }
        ++result.owned;
        if (bestAlbum < 0 || data.records[track.owner->record].album != bestAlbum) {
            ++result.ownedAsSingles;
        }
    }
}

} // namespace

AlbumMatchResult AlbumMatcher::match(const QList<RemoteAlbumFile> &files, MatchOptions options) const
{
    AlbumMatchResult result;
    result.trackTiers = QList<Tier>(files.size(), Tier::None);
    QStringList *explanation = options.explain ? &result.explanation : nullptr;
    if (!m_index) {
        if (explanation != nullptr) {
            explanation->append(u"no library snapshot: nothing can match"_s);
        }
        return result;
    }
    const detail::IndexData &data = m_index->data();

    std::vector<int> lengths;
    const std::vector<RemoteTrack> tracks = prepareTracks(data, files, result, lengths);
    const RemoteAlbum remote = remoteAlbum(files, tracks);
    if (explanation != nullptr) {
        explanation->append(u"%1 audio file(s); album '%2' by '%3'%4%5"_s.arg(result.remoteTracks)
                                .arg(remote.title, remote.artist,
                                     remote.year > 0 ? u" (%1)"_s.arg(remote.year) : QString(),
                                     remote.variousArtists ? u", various artists"_s : QString()));
    }

    // Candidates: the albums of the tracks owned, albums named like the folder, the duration signature.
    Candidates candidates(data.albums.size());
    addOwnerAlbums(data, tracks, candidates);
    addNamedAlbums(data, remote, candidates);
    addSignatureAlbums(data, lengths, candidates);

    std::optional<Evaluation> best;
    for (const qint32 album : candidates.list()) {
        Evaluation evaluation = evaluate(data, album, tracks, remote, lengths);
        if (explanation != nullptr) {
            explanation->append(evaluation.explanation);
        }
        if (evaluation.match.found && (!best || betterAlbum(evaluation.match, best->match))) {
            best = std::move(evaluation);
        }
    }
    if (best) {
        result.best = best->match;
    }

    countCoverage(data, tracks, best ? best->album : -1, result);
    if (explanation != nullptr) {
        explanation->append(best ? u"best: album %1, %2"_s.arg(result.best.albumId)
                                       .arg(result.best.inCollection
                                                ? u"in collection ("_s + tierName(result.best.tier) + u')'
                                                : u"not in collection"_s)
                                 : u"no local album matches"_s);
        explanation->append(
            u"owned %1/%2 (%3 as singles)"_s.arg(result.owned).arg(result.remoteTracks).arg(result.ownedAsSingles));
    }
    return result;
}

} // namespace selecta::match
