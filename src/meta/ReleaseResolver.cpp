// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/ReleaseResolver.h"

#include "match/Similarity.h"
#include "match/VersionClassifier.h"
#include "meta/DeezerClient.h"
#include "meta/SpotifyClient.h"
#include "meta/detail/Async.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/TitleInfo.h"

#include <QHash>
#include <QMetaObject>
#include <QSet>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::meta {

namespace {

// Weights and thresholds of the note §14.5 (initial values; experiment X5 tunes them on a labelled corpus).
constexpr double kTitleWeight = 0.40;
constexpr double kArtistWeight = 0.30;
constexpr double kDurationWeight = 0.20;
constexpr double kAlbumWeight = 0.05;
constexpr double kLabelWeight = 0.05;
constexpr double kExactWindowSec = 3.0; // D23
constexpr double kDjMixFactor = 0.3;
constexpr double kJunkFactor = 0.2;
constexpr double kOtherArtistFactor = 0.5;
constexpr double kUnsureVersionFactor = 0.9;
constexpr double kConfidentScore = 0.92;
constexpr double kConfidentMargin = 0.05;
constexpr double kLikelyScore = 0.80;
constexpr double kAmbiguousScore = 0.60;
constexpr double kIsrcTitleFloor = 0.6;
constexpr double kExplainedArtist = 0.8;

QString percent(double value)
{
    return QString::number(value, 'f', 2);
}

double fieldSimilarity(const QString &a, const QString &b, bool corroborated = false)
{
    if (a.isEmpty() || b.isEmpty()) {
        return 0.0;
    }
    match::FieldMatchOptions options;
    options.durationCorroborates = corroborated;
    return match::FieldMatcher::compareKeys(a, b, options).similarity;
}

double bestSimilarity(const QString &key, const QStringList &against)
{
    double best = 0.0;
    for (const QString &other : against) {
        best = std::max(best, fieldSimilarity(key, other));
    }
    return best;
}

Confidence tierFor(double score)
{
    if (score >= kConfidentScore) {
        return Confidence::Confident;
    }
    if (score >= kLikelyScore) {
        return Confidence::Likely;
    }
    if (score >= kAmbiguousScore) {
        return Confidence::Ambiguous;
    }
    return Confidence::None;
}

// The artist similarity of §14.5: 0.7 for the line's primary artists found among the release's, 0.3 for the rest —
// the line's featured artists found there, or else the share of the release's artists the line explains (its
// remixer counts).
std::optional<double> artistSimilarity(const ResolveQuery &query, const detail::TitleInfo &queryTitle,
                                       const QStringList &releaseArtists)
{
    const match::ArtistKeys line = match::Normalizer::artistKeys(query.artist);
    if (line.set.isEmpty() || releaseArtists.isEmpty()) {
        return std::nullopt; // nothing to compare: the weight leaves the denominator
    }
    double primary = 0.0;
    for (const QString &artist : line.set) {
        primary += bestSimilarity(artist, releaseArtists);
    }
    primary /= static_cast<double>(line.set.size());

    QStringList featured = line.featured;
    for (const QString &artist : queryTitle.featured) {
        if (!featured.contains(artist)) {
            featured.append(artist);
        }
    }
    double rest = 1.0;
    if (!featured.isEmpty()) {
        rest = 0.0;
        for (const QString &artist : featured) {
            rest += bestSimilarity(artist, releaseArtists);
        }
        rest /= static_cast<double>(featured.size());
    } else {
        QStringList explained = line.set;
        explained += queryTitle.version.by;
        qsizetype unexplained = 0;
        for (const QString &artist : releaseArtists) {
            if (bestSimilarity(artist, explained) < kExplainedArtist) {
                ++unexplained;
            }
        }
        rest = 1.0 - (0.5 * static_cast<double>(unexplained) / static_cast<double>(releaseArtists.size()));
    }
    return (0.7 * primary) + (0.3 * rest);
}

bool versionCompatible(match::VersionCompatibility version)
{
    return version == match::VersionCompatibility::Same || version == match::VersionCompatibility::ProbablySame;
}

// What scoring needs from the line, worked out once per line.
struct LineFacts
{
    explicit LineFacts(ResolveQuery line)
        : query(std::move(line))
        , title(detail::analyseTitle(query.title, query.version))
        , isrc(detail::normalizedIsrc(query.isrc))
    {}

    ResolveQuery query;
    detail::TitleInfo title;
    QString isrc;
};

// A scored release, with the keys that the merge and the Confident margin compare.
struct Scored
{
    Candidate candidate;
    QString coreKey;
    QString versionKey;
};

// The length difference in seconds, when both sides know their length.
std::optional<double> lengthDelta(const ResolveQuery &query, const Release &release)
{
    if (!query.durationMs.has_value() || !release.durationMs.has_value() || *query.durationMs <= 0 ||
        *release.durationMs <= 0) {
        return std::nullopt;
    }
    return std::abs(static_cast<double>(*query.durationMs - *release.durationMs)) / 1000.0;
}

// The score of §14.5 for one release: weighted field similarities over the fields both sides have, then the version,
// DJ-mix, derivative and namesake penalties. The ISRC rule makes it Exact.
Scored scoreRelease(const LineFacts &line, const Release &release)
{
    const ResolveQuery &query = line.query;
    const detail::TitleInfo &lineTitle = line.title;
    const detail::TitleInfo releaseTitle = detail::analyseTitle(release.title);
    Scored scored;
    scored.coreKey = releaseTitle.core.key;
    scored.versionKey = releaseTitle.version.key();
    Candidate &candidate = scored.candidate;
    candidate.release = release;
    candidate.version = match::VersionClassifier::compare(lineTitle.version, releaseTitle.version);

    const std::optional<double> deltaSec = lengthDelta(query, release);
    const bool lengthAgrees = deltaSec.has_value() && *deltaSec <= kExactWindowSec;

    double weighted = 0.0;
    double weights = 0.0;
    QStringList parts;
    double titleScore = 0.0;
    if (!lineTitle.core.key.isEmpty() && !releaseTitle.core.key.isEmpty()) {
        titleScore = fieldSimilarity(lineTitle.core.key, releaseTitle.core.key, lengthAgrees);
        weighted += kTitleWeight * titleScore;
        weights += kTitleWeight;
        parts.append(u"title "_s + percent(titleScore));
    }
    const std::optional<double> artistScore =
        artistSimilarity(query, lineTitle, detail::releaseArtistKeys(release, releaseTitle));
    if (artistScore.has_value()) {
        weighted += kArtistWeight * *artistScore;
        weights += kArtistWeight;
        parts.append(u"artist "_s + percent(*artistScore));
    }
    if (deltaSec.has_value() && candidate.version != match::VersionCompatibility::OtherVersion) {
        const double durationScore = lengthAgrees ? 1.0 : std::exp(-0.1 * (*deltaSec - kExactWindowSec));
        weighted += kDurationWeight * durationScore;
        weights += kDurationWeight;
        parts.append(u"length Δ %1 s"_s.arg(qRound(*deltaSec)));
    }
    if (!query.album.isEmpty() && !release.album.isEmpty()) {
        const double albumScore = fieldSimilarity(match::Normalizer::albumKey(query.album).key,
                                                  match::Normalizer::albumKey(release.album).key);
        weighted += kAlbumWeight * albumScore;
        weights += kAlbumWeight;
        parts.append(u"album "_s + percent(albumScore));
    }
    if (!query.label.isEmpty() && !release.label.isEmpty()) {
        const double labelScore = fieldSimilarity(match::Normalizer::normalize(query.label),
                                                  match::Normalizer::normalize(release.label)) >= 0.85
                                      ? 1.0
                                      : 0.0;
        weighted += kLabelWeight * labelScore;
        weights += kLabelWeight;
        parts.append(labelScore > 0 ? u"label agrees"_s : u"label differs"_s);
    }
    double score = weights > 0 ? weighted / weights : 0.0;
    if (!parts.isEmpty()) {
        candidate.reasons.append(parts.join(u" · "_s));
    }

    if (candidate.version == match::VersionCompatibility::OtherVersion) {
        score = 0.0;
        candidate.reasons.append(u"other version: %1 vs %2"_s.arg(
            lineTitle.version.raw.isEmpty() ? u"none"_s : lineTitle.version.raw,
            releaseTitle.version.raw.isEmpty() ? u"none"_s : releaseTitle.version.raw));
    }
    if (releaseTitle.djMix && !lineTitle.djMix) {
        score *= kDjMixFactor;
        candidate.reasons.append(u"DJ-mix segment ×%1"_s.arg(kDjMixFactor));
    }
    const bool junkAsked =
        std::ranges::all_of(releaseTitle.junk, [&](const QString &marker) { return lineTitle.junk.contains(marker); });
    if (!releaseTitle.junk.isEmpty() && !junkAsked) {
        score *= kJunkFactor;
        candidate.reasons.append(u"derivative (%1) ×%2"_s.arg(releaseTitle.junk.join(u", "_s)).arg(kJunkFactor));
    }
    if (artistScore.has_value() && titleScore >= 0.9 && *artistScore < 0.5) {
        score *= kOtherArtistFactor;
        candidate.reasons.append(u"same title, another artist ×%1"_s.arg(kOtherArtistFactor));
    }
    if (candidate.version == match::VersionCompatibility::Unsure && !deltaSec.has_value()) {
        score *= kUnsureVersionFactor;
        candidate.reasons.append(u"version not named on the line ×%1"_s.arg(kUnsureVersionFactor));
    }
    candidate.score = std::clamp(score, 0.0, 1.0);

    const bool isrcRule = !line.isrc.isEmpty() && line.isrc == release.isrc &&
                          (!deltaSec.has_value() || lengthAgrees) &&
                          (lineTitle.core.key.isEmpty() || titleScore >= kIsrcTitleFloor);
    if (isrcRule) {
        candidate.confidence = Confidence::Exact;
        candidate.reasons.prepend(u"ISRC %1 matches"_s.arg(line.isrc));
    } else {
        candidate.confidence = tierFor(candidate.score);
    }
    return scored;
}

// The better of two candidates: an exact one first, then the higher score.
bool better(const Candidate &a, const Candidate &b)
{
    if ((a.confidence == Confidence::Exact) != (b.confidence == Confidence::Exact)) {
        return a.confidence == Confidence::Exact;
    }
    return a.score > b.score;
}

bool isCompilation(const Release &release)
{
    return release.albumType.compare(u"compilation"_s, Qt::CaseInsensitive) == 0;
}

// The copy whose album a merged candidate shows: Spotify's (the primary source), then an album or single over a
// compilation, then the earlier release.
bool betterBase(const Release &a, const Release &b)
{
    if ((a.source == Source::Spotify) != (b.source == Source::Spotify)) {
        return a.source == Source::Spotify;
    }
    if (isCompilation(a) != isCompilation(b)) {
        return !isCompilation(a);
    }
    return !a.releaseDate.isEmpty() && !b.releaseDate.isEmpty() && a.releaseDate < b.releaseDate;
}

// Adds what `copy` knows and `kept` lacks: the other service's IDs, the label, covers.
void fillIn(Release &kept, const Release &copy)
{
    if (kept.deezerId.isEmpty() && !copy.deezerId.isEmpty()) {
        kept.deezerId = copy.deezerId;
        kept.deezerUrl = copy.deezerUrl;
        kept.deezerAlbumId = copy.deezerAlbumId;
    }
    if (kept.spotifyId.isEmpty() && !copy.spotifyId.isEmpty()) {
        kept.spotifyId = copy.spotifyId;
        kept.spotifyUrl = copy.spotifyUrl;
        kept.spotifyAlbumId = copy.spotifyAlbumId;
    }
    if (kept.label.isEmpty() && !copy.label.isEmpty()) {
        kept.label = copy.label;
        kept.labelSource = copy.labelSource;
    }
    if (kept.covers.isEmpty()) {
        kept.covers = copy.covers;
    }
}

// One candidate per recording: copies with the same ISRC and version merge; the better copy decides the score.
QList<Scored> merged(const QList<Scored> &scored)
{
    QList<Scored> out;
    out.reserve(scored.size());
    for (const Scored &one : scored) {
        const Release &release = one.candidate.release;
        const auto duplicate = std::ranges::find_if(out, [&](const Scored &kept) {
            return !release.isrc.isEmpty() && kept.candidate.release.isrc == release.isrc &&
                   kept.versionKey == one.versionKey;
        });
        if (duplicate == out.end()) {
            out.append(one);
            continue;
        }
        Candidate &kept = duplicate->candidate;
        if (betterBase(release, kept.release)) {
            Release base = release;
            fillIn(base, kept.release);
            kept.release = std::move(base);
        } else {
            fillIn(kept.release, release);
        }
        if (better(one.candidate, kept)) {
            kept.score = one.candidate.score;
            kept.confidence = one.candidate.confidence;
            kept.version = one.candidate.version;
            kept.reasons = one.candidate.reasons;
        }
    }
    return out;
}

// A recording or version different from another candidate's: the Confident margin is measured against those only.
bool differentRecording(const Scored &a, const Scored &b)
{
    if (!a.candidate.release.isrc.isEmpty() && !b.candidate.release.isrc.isEmpty()) {
        return a.candidate.release.isrc != b.candidate.release.isrc;
    }
    return a.versionKey != b.versionKey || a.coreKey != b.coreKey;
}

// Tiers for candidates sorted best first. Confident only for the best, and only with a margin over every other
// recording or version; two sources agreeing on the best one's ISRC raise it one tier, but never past a missing margin.
void assignTiers(QList<Scored> &sorted, const QHash<QString, QSet<Source>> &isrcSources)
{
    for (qsizetype i = 0; i < sorted.size(); ++i) {
        Candidate &candidate = sorted[i].candidate;
        if (candidate.confidence == Confidence::Exact) {
            continue;
        }
        const bool leads = i == 0 && std::ranges::none_of(sorted, [&](const Scored &other) {
                               return &other != &sorted.at(0) && differentRecording(sorted.at(0), other) &&
                                      other.candidate.score > candidate.score - kConfidentMargin;
                           });
        Confidence tier = tierFor(candidate.score);
        if (tier == Confidence::Confident && !leads) {
            tier = Confidence::Likely;
        }
        const bool agreed = !candidate.release.isrc.isEmpty() && isrcSources.value(candidate.release.isrc).size() >= 2;
        if (leads && agreed && (tier == Confidence::Ambiguous || tier == Confidence::Likely)) {
            tier = tier == Confidence::Ambiguous ? Confidence::Likely : Confidence::Confident;
            candidate.reasons.append(u"ISRC agreed by two sources"_s);
        }
        candidate.confidence = tier;
    }
}

Resolution combine(const QList<Scored> &scored)
{
    QHash<QString, QSet<Source>> isrcSources; // which sources carry each ISRC
    for (const Scored &one : scored) {
        if (!one.candidate.release.isrc.isEmpty()) {
            isrcSources[one.candidate.release.isrc].insert(one.candidate.release.source);
        }
    }
    QList<Scored> candidates = merged(scored);
    std::ranges::stable_sort(candidates,
                             [](const Scored &a, const Scored &b) { return better(a.candidate, b.candidate); });
    assignTiers(candidates, isrcSources);
    Resolution resolution;
    resolution.candidates.reserve(candidates.size());
    for (Scored &one : candidates) {
        resolution.candidates.append(std::move(one.candidate));
    }
    resolution.confidence =
        resolution.candidates.isEmpty() ? Confidence::None : resolution.candidates.first().confidence;
    return resolution;
}

} // namespace

QString confidenceName(Confidence confidence)
{
    switch (confidence) {
    case Confidence::None:
        return u"none"_s;
    case Confidence::Ambiguous:
        return u"ambiguous"_s;
    case Confidence::Likely:
        return u"likely"_s;
    case Confidence::Confident:
        return u"confident"_s;
    case Confidence::Exact:
        return u"exact"_s;
    }
    return u"none"_s;
}

Candidate ReleaseResolver::score(const ResolveQuery &query, const Release &release)
{
    return scoreRelease(LineFacts(query), release).candidate;
}

Resolution ReleaseResolver::rank(const ResolveQuery &query, const QList<Release> &releases)
{
    const LineFacts line(query);
    QList<Scored> scored;
    scored.reserve(releases.size());
    for (const Release &release : releases) {
        scored.append(scoreRelease(line, release));
    }
    return combine(scored);
}

// One resolution, step by step (identifiers, Spotify, Deezer). Kept alive by the step in flight only. Each release is
// scored once, as it arrives; every step then ranks the lot once.
class ReleaseResolver::Job : public std::enable_shared_from_this<Job>
{
public:
    Job(ReleaseResolver *resolver, const ResolveQuery &query, Priority priority,
        detail::SharedPromise<Result<Resolution>> promise, detail::CancelSlot slot)
        : m_resolver(resolver)
        , m_line(query)
        , m_priority(priority)
        , m_promise(std::move(promise))
        , m_slot(std::move(slot))
        , m_artist(detail::primaryArtist(query.artist))
        , m_versionWords(detail::versionWords(m_line.title.version))
    {}

    void start()
    {
        if (!m_line.isrc.isEmpty()) {
            spotifyIsrc();
            return;
        }
        textStage();
    }

private:
    template <typename T>
    using Then = std::function<void(const Result<T> &)>;

    // A source that failed for this line (rate limit, network, …) is not asked again: the next request would fail the
    // same way, and a struggling service deserves a break.
    bool usable(Source source) const
    {
        if (m_failed.contains(source)) {
            return false;
        }
        if (source == Source::Spotify) {
            return m_resolver->m_spotify != nullptr && m_resolver->m_spotify->isAvailable();
        }
        return m_resolver->m_deezer != nullptr;
    }

    template <typename T>
    void await(const QFuture<Result<T>> &future, Source source, Then<T> then)
    {
        m_slot.set([pending = QFuture<Result<T>>(future)]() mutable { pending.cancel(); });
        if (!m_tried.contains(source)) {
            m_tried.append(source);
        }
        const auto self = shared_from_this();
        const auto callback = std::make_shared<Then<T>>(std::move(then));
        detail::whenReady(future, m_resolver, [self, source, callback](const Result<T> &result) {
            self->m_slot.clear();
            if (self->m_slot.isCancelled()) {
                return;
            }
            if (result.ok() || result.error().code == ErrorCode::NotFound) {
                self->m_answered = true; // "no such ISRC" is an answer too
            } else {
                self->m_errors.append(result.error());
                self->m_failed.insert(source);
            }
            (*callback)(result);
        });
    }

    void add(const Release &release) { m_scored.append(scoreRelease(m_line, release)); }

    void add(const QList<Release> &releases)
    {
        for (const Release &release : releases) {
            add(release);
        }
    }

    void add(const QList<DeezerTrack> &tracks)
    {
        for (const DeezerTrack &track : tracks) {
            add(track.release);
        }
    }

    void spotifyIsrc()
    {
        if (!usable(Source::Spotify)) {
            deezerIsrc();
            return;
        }
        await<QList<Release>>(m_resolver->m_spotify->searchIsrc(m_line.isrc, m_priority), Source::Spotify,
                              [this](const Result<QList<Release>> &result) {
                                  if (result.ok()) {
                                      add(result.value());
                                  }
                                  deezerIsrc();
                              });
    }

    void deezerIsrc()
    {
        if (!usable(Source::Deezer)) {
            afterIsrc();
            return;
        }
        await<DeezerTrack>(m_resolver->m_deezer->trackByIsrc(m_line.isrc, false, m_priority), Source::Deezer,
                           [this](const Result<DeezerTrack> &result) {
                               if (result.ok()) {
                                   add(result.value().release);
                               }
                               afterIsrc();
                           });
    }

    void afterIsrc()
    {
        Resolution now = combine(m_scored);
        if (now.confidence == Confidence::Exact) {
            finish(std::move(now));
            return;
        }
        textStage();
    }

    void textStage()
    {
        if (m_line.title.coreDisplay.isEmpty() && m_artist.isEmpty()) {
            finish(combine(m_scored));
            return;
        }
        if (usable(Source::Spotify)) {
            spotifySearch(false);
            return;
        }
        deezerSearch(false);
    }

    void spotifySearch(bool withVersion)
    {
        TrackQuery search;
        search.artist = m_artist;
        search.title = m_line.title.coreDisplay;
        search.version = withVersion ? m_versionWords : QString();
        search.limit = SpotifyClient::kMaxSearchLimit;
        search.priority = m_priority;
        await<QList<Release>>(m_resolver->m_spotify->searchTracks(search), Source::Spotify,
                              [this, withVersion](const Result<QList<Release>> &result) {
                                  const bool found = result.ok() && !result.value().isEmpty();
                                  if (found) {
                                      add(result.value());
                                  }
                                  Resolution now = combine(m_scored);
                                  if (goodEnough(now)) {
                                      finish(std::move(now));
                                  } else if (!withVersion && found && wantsVersionSearch(now)) {
                                      spotifySearch(true);
                                  } else {
                                      deezerSearch(false);
                                  }
                              });
    }

    void deezerSearch(bool withVersion)
    {
        if (!usable(Source::Deezer)) {
            finish(combine(m_scored));
            return;
        }
        QString text = m_artist + u' ' + m_line.title.coreDisplay;
        if (withVersion) {
            text += u' ' + m_versionWords;
        }
        await<QList<DeezerTrack>>(m_resolver->m_deezer->search(text.trimmed(), kDeezerSearchLimit, m_priority),
                                  Source::Deezer, [this, withVersion](const Result<QList<DeezerTrack>> &result) {
                                      const bool found = result.ok() && !result.value().isEmpty();
                                      if (found) {
                                          add(result.value());
                                      }
                                      Resolution now = combine(m_scored);
                                      if (!goodEnough(now) && !withVersion && found && wantsVersionSearch(now)) {
                                          deezerSearch(true);
                                      } else {
                                          finish(std::move(now));
                                      }
                                  });
    }

    // A Likely (or better) candidate whose version agrees: no need to ask further.
    static bool goodEnough(const Resolution &now)
    {
        return !now.candidates.isEmpty() && now.confidence >= Confidence::Likely &&
               (now.confidence == Confidence::Exact || versionCompatible(now.candidates.first().version));
    }

    // The line names a version and no candidate of that version came back: search again with its words. (Not after an
    // empty first page: more words only narrow the search.)
    bool wantsVersionSearch(const Resolution &now) const
    {
        return !m_versionWords.isEmpty() && std::ranges::none_of(now.candidates, [](const Candidate &candidate) {
            return versionCompatible(candidate.version) && candidate.score >= kAmbiguousScore;
        });
    }

    void finish(Resolution resolution)
    {
        if (!m_answered && !m_errors.isEmpty()) {
            m_promise.finish(m_errors.first()); // every source failed
            return;
        }
        resolution.sourcesTried = m_tried;
        resolution.sourceErrors = m_errors;
        m_promise.finish(std::move(resolution));
    }

    static constexpr int kDeezerSearchLimit = 25;

    ReleaseResolver *m_resolver;
    LineFacts m_line;
    Priority m_priority;
    detail::SharedPromise<Result<Resolution>> m_promise;
    detail::CancelSlot m_slot;
    QString m_artist;
    QString m_versionWords;
    QList<Scored> m_scored;
    QList<Source> m_tried;
    QList<MetaError> m_errors;
    QSet<Source> m_failed;
    bool m_answered = false;
};

ReleaseResolver::ReleaseResolver(SpotifyClient *spotify, DeezerClient *deezer, QObject *parent)
    : QObject(parent)
    , m_spotify(spotify)
    , m_deezer(deezer)
{}

ReleaseResolver::~ReleaseResolver() = default;

QFuture<Result<Resolution>> ReleaseResolver::resolve(const ResolveQuery &query, Priority priority)
{
    const detail::SharedPromise<Result<Resolution>> promise;
    const detail::CancelSlot slot;
    promise.onCanceled(this, [slot] { slot.cancel(); });
    if (query.artist.trimmed().isEmpty() && query.title.trimmed().isEmpty() &&
        detail::normalizedIsrc(query.isrc).isEmpty()) {
        promise.finishLater(this, makeError(Service::Deezer, ErrorCode::InvalidArgument,
                                            u"A line needs an artist, a title or an ISRC."_s));
        return promise.future();
    }
    if (m_spotify == nullptr && m_deezer == nullptr) {
        promise.finishLater(
            this, makeError(Service::Deezer, ErrorCode::NotConfigured, u"No metadata source is configured."_s));
        return promise.future();
    }
    const auto job = std::make_shared<Job>(this, query, priority, promise, slot);
    QMetaObject::invokeMethod(this, [job] { job->start(); }, Qt::QueuedConnection);
    return promise.future();
}

} // namespace selecta::meta
