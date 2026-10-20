// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/ReleaseScorer.h"

#include "match/Duration.h"
#include "match/Normalizer.h"
#include "match/VersionClassifier.h"
#include "match/detail/Alignment.h"
#include "match/detail/Scoring.h"

#include <algorithm>
#include <numeric>
#include <tuple>

using namespace Qt::StringLiterals;

namespace selecta::match {

struct ReleaseScorer::Prepared
{
    ReleaseReference reference;
    TrackKeys keys;             // title and album keys of the release
    QStringList artistKeys;     // each primary artist's key (storage for `artist`)
    detail::AlignTokens title;  // views into keys
    detail::AlignTokens artist; // views into artistKeys
};

namespace {

constexpr double kEpsilon = 1e-9;

// A remix, edit or other DJ version in the file that the release doesn't have is a conflict (note §17.5 B).
bool versionConflicts(const VersionInfo &file, const VersionInfo &release)
{
    const VersionCompatibility compatibility = VersionClassifier::compare(file, release);
    if (compatibility == VersionCompatibility::OtherVersion) {
        return true;
    }
    const bool releasePlain =
        release.versionClass == VersionClass::Unspecified || release.versionClass == VersionClass::Original;
    switch (file.versionClass) {
    case VersionClass::Remix:
    case VersionClass::Vip:
    case VersionClass::Dub:
    case VersionClass::Instrumental:
    case VersionClass::Acapella:
    case VersionClass::Live:
    case VersionClass::Acoustic:
    case VersionClass::Demo:
        return releasePlain;
    case VersionClass::Unspecified:
    case VersionClass::Original:
    case VersionClass::Extended:
    case VersionClass::Radio:
    case VersionClass::Vocal:
        return false; // the length decides between an original, its extended and its radio edit
    }
    return false;
}

int compatibilityRank(VersionCompatibility compatibility)
{
    switch (compatibility) {
    case VersionCompatibility::Same:
        return 3;
    case VersionCompatibility::ProbablySame:
        return 2;
    case VersionCompatibility::Unsure:
        return 1;
    case VersionCompatibility::OtherVersion:
        return 0;
    }
    return 0;
}

QString number(double value)
{
    return QString::number(value, 'f', 2);
}

void explainFit(ReleaseFit &fit, const ReleaseReference &reference, const DurationComparison &duration, bool conflict)
{
    fit.explanation.append(
        u"release: '%1' by %2%3"_s.arg(reference.title, reference.artists.join(u", "),
                                       duration.known ? u", Δ %1 s"_s.arg(duration.delta) : u", length unknown"_s));
    fit.explanation.append(u"title coverage %1, artist coverage %2, unexplained %3, length %4"_s.arg(
        number(fit.titleCoverage), number(fit.artistCoverage), number(fit.unexplained),
        fit.duration ? number(*fit.duration) : u"—"_s));
    fit.explanation.append(
        u"version %1%2%3 → fit %4: %5"_s.arg(compatibilityName(fit.version), conflict ? u" (conflict)"_s : QString(),
                                             fit.artistOnlyInBrackets ? u", artist only as a remixer"_s : QString(),
                                             number(fit.fit), fit.fits ? u"fits"_s : u"does not fit"_s));
}

} // namespace

ReleaseScorer::ReleaseScorer(const ReleaseReference &reference)
{
    auto prepared = std::make_shared<Prepared>();
    prepared->reference = reference;
    prepared->keys = Normalizer::trackKeys(reference.artists.join(u" & "), reference.title, reference.album);
    for (const QString &artist : reference.artists) {
        QString key = Normalizer::normalize(artist, FieldKind::Artist);
        if (!key.isEmpty()) {
            prepared->artistKeys.append(std::move(key));
        }
    }
    detail::appendAlignTokens(prepared->title, prepared->keys.title.field.key);
    for (qsizetype i = 0; i < prepared->artistKeys.size(); ++i) {
        detail::appendAlignTokens(prepared->artist, prepared->artistKeys.at(i), static_cast<int>(i));
    }
    m_prepared = std::move(prepared);
}

const ReleaseReference &ReleaseScorer::reference() const
{
    return m_prepared->reference;
}

ReleaseFit ReleaseScorer::score(const ParsedRemote &parsed, int durationSec, bool explain) const
{
    const Prepared &release = *m_prepared;
    ReleaseFit fit;
    RemoteFile file;
    file.attributes.durationSec = durationSec;
    const detail::Query query = detail::prepareRemote(parsed, file);

    const DurationComparison duration = compareRemoteDuration(durationSec, release.reference.durationMs);
    if (duration.known) {
        fit.duration = duration.score;
    }
    fit.lengthUnknown = !duration.known;
    const FieldMatchOptions options{duration.withinExact()};

    const detail::ZoneBag bag = detail::zoneBag(query, release.title, release.artist, options);
    fit.titleCoverage = bag.coverageTitle;
    fit.artistCoverage = bag.coverageArtist;
    fit.unexplained = bag.unexplained;
    fit.artistOnlyInBrackets = bag.artistOnlyInBrackets;
    fit.fuzzy = bag.fuzzy;

    fit.version = VersionClassifier::compare(parsed.version, release.keys.title.version);
    const bool conflict = versionConflicts(parsed.version, release.keys.title.version);

    double weighted = (kFitWeightTitle * fit.titleCoverage) + (kFitWeightArtist * fit.artistCoverage);
    double weights = kFitWeightTitle + kFitWeightArtist;
    if (fit.duration) {
        weighted += kFitWeightDuration * *fit.duration;
        weights += kFitWeightDuration;
    }
    double multiplier = 1.0;
    if (conflict) {
        multiplier *= 0.0;
    }
    if (fit.artistOnlyInBrackets) {
        multiplier *= kArtistOnlyInBracketsFactor;
    }
    if (fit.unexplained > kMaxUnexplained) {
        multiplier *= kUnexplainedFactor;
    }
    fit.fit = (weighted / weights) * multiplier;
    const bool lengthFits = fit.lengthUnknown || (fit.duration && *fit.duration >= 1.0);
    fit.fits = fit.fit >= kOnlyThisReleaseMinFit - kEpsilon && lengthFits;

    if (explain) {
        explainFit(fit, release.reference, duration, conflict);
    }
    return fit;
}

QList<qsizetype> ReleaseScorer::rank(const QList<RankedFile> &files)
{
    QList<qsizetype> order(files.size());
    std::iota(order.begin(), order.end(), qsizetype{0});
    std::ranges::stable_sort(order, [&files](qsizetype x, qsizetype y) {
        const RankedFile &a = files.at(x);
        const RankedFile &b = files.at(y);
        if (const int identity = compareIdentity(a.fit, b.fit); identity != 0) {
            return identity < 0;
        }
        if (a.qualityRank != b.qualityRank) {
            return a.qualityRank > b.qualityRank;
        }
        if (a.freeSlot != b.freeSlot) {
            return a.freeSlot;
        }
        if (a.queueLength != b.queueLength) {
            return a.queueLength < b.queueLength;
        }
        if (a.peerLoad != b.peerLoad) {
            return a.peerLoad < b.peerLoad; // spread what we take over peers (FAIR-5)
        }
        return std::tie(a.peer, a.path) < std::tie(b.peer, b.path);
    });
    return order;
}

int ReleaseScorer::compareIdentity(const ReleaseFit &a, const ReleaseFit &b)
{
    // Length first (D23): a known length ranks above an unknown one at equal fit status.
    const auto key = [](const ReleaseFit &f) {
        return std::make_tuple(f.fits, f.duration.value_or(-1.0), f.artistCoverage, f.titleCoverage,
                               compatibilityRank(f.version), f.fit);
    };
    const auto ka = key(a);
    const auto kb = key(b);
    if (ka == kb) {
        return 0;
    }
    return ka > kb ? -1 : 1;
}

} // namespace selecta::match
