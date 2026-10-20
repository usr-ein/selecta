// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Supersede.h"

#include "match/Duration.h"
#include "match/MatchIndex.h"
#include "match/Matcher.h"
#include "match/Normalizer.h"
#include "match/Quality.h"
#include "match/VersionClassifier.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace {

constexpr qsizetype kMatchesPerSingle = 4;

LibraryTrack libraryTrack(quint64 id, const LocalTrackFacts &facts)
{
    LibraryTrack track;
    track.id = id;
    track.source = Source::Collection; // where the album copy sits doesn't matter to the comparison
    track.artist = facts.artist;
    track.title = facts.title;
    track.durationMs = facts.durationMs;
    track.quality = facts.quality;
    return track;
}

LocalTrackQuery localQuery(const LocalTrackFacts &facts)
{
    LocalTrackQuery query;
    query.artist = facts.artist;
    query.title = facts.title;
    query.durationMs = facts.durationMs;
    query.quality = facts.quality;
    return query;
}

QString display(const LocalTrackFacts &facts)
{
    const QString artist = facts.artist.trimmed();
    const QString title = facts.title.trimmed();
    return artist.isEmpty() ? u"“%1”"_s.arg(title) : u"“%1 – %2”"_s.arg(artist, title);
}

QString codecLabel(Codec codec)
{
    switch (codec) {
    case Codec::Mp3:
        return u"MP3"_s;
    case Codec::Aac:
        return u"AAC"_s;
    case Codec::Vorbis:
        return u"Vorbis"_s;
    case Codec::Opus:
        return u"Opus"_s;
    case Codec::Wma:
        return u"WMA"_s;
    case Codec::Flac:
        return u"FLAC"_s;
    case Codec::Alac:
        return u"ALAC"_s;
    case Codec::Wav:
        return u"WAV"_s;
    case Codec::Aiff:
        return u"AIFF"_s;
    case Codec::Ape:
        return u"APE"_s;
    case Codec::WavPack:
        return u"WavPack"_s;
    case Codec::Unknown:
        break;
    }
    return u"unknown format"_s;
}

// "FLAC 24-bit", "MP3 320", "AAC 256", for the activity log.
QString describe(const AudioQuality &quality)
{
    if (isLossless(quality.codec)) {
        return u"%1 %2-bit"_s.arg(codecLabel(quality.codec)).arg(quality.bitDepth > 0 ? quality.bitDepth : 16);
    }
    if (quality.bitrateKbps > 0) {
        return u"%1 %2"_s.arg(codecLabel(quality.codec)).arg(quality.bitrateKbps);
    }
    return codecLabel(quality.codec);
}

// What the matcher and the strict rule say about two local files.
struct PairMatch
{
    bool found = false;        // tier ≥ likely, or another version
    bool otherVersion = false; // the same title in another version (T40)
    Tier tier = Tier::None;
    quint64 recordId = 0; // the matched record (the album track, or the existing copy)
    QStringList explanation;
};

// The strict local-against-local exact rule (note §15): equal title keys (and bracket extras), equal artist sets,
// the same version, both lengths known and within 2 s.
bool strictExact(const LocalTrackFacts &a, const LocalTrackFacts &b, QStringList &explanation)
{
    const TrackKeys keysA = Normalizer::trackKeys(a.artist, a.title, {});
    const TrackKeys keysB = Normalizer::trackKeys(b.artist, b.title, {});
    const bool titles = !keysA.title.field.squashed.isEmpty() &&
                        keysA.title.field.squashed == keysB.title.field.squashed &&
                        keysA.title.extraKey == keysB.title.extraKey;
    const bool artists = !keysA.artist.set.isEmpty() && keysA.artist.set == keysB.artist.set;
    const bool version =
        VersionClassifier::compare(keysA.title.version, keysB.title.version) == VersionCompatibility::Same;
    const DurationComparison lengths = compareDurations(a.durationMs, b.durationMs);
    const bool length = lengths.known && lengths.delta <= kLocalExactWindowSec;
    const auto yesNo = [](bool value) { return value ? u"yes"_s : u"no"_s; };
    explanation.append(u"exact (local): title keys equal %1, artist sets equal %2, same version %3, lengths within "
                       u"%4 s %5"_s.arg(yesNo(titles), yesNo(artists), yesNo(version))
                           .arg(kLocalExactWindowSec)
                           .arg(lengths.known ? u"%1 (Δ %2 s)"_s.arg(yesNo(length)).arg(lengths.delta, 0, 'f', 1)
                                              : u"no (a length is unknown)"_s));
    return titles && artists && version && length;
}

// The best record of `matcher`'s snapshot for a local file by the §9 rules with exact lengths, then the strict rule.
PairMatch compareLocal(const Matcher &matcher, const LocalTrackFacts &facts,
                       const QHash<quint64, const LocalTrackFacts *> &records)
{
    PairMatch pair;
    const QList<RecordMatch> matches = matcher.matchLocal(localQuery(facts), kMatchesPerSingle);
    if (matches.isEmpty()) {
        pair.explanation.append(u"no match by the §9 rules"_s);
        return pair;
    }
    const RecordMatch &best = matches.front();
    const LocalTrackFacts *other = records.value(best.id, nullptr);
    if (other == nullptr) {
        return pair; // can't happen: every record of the snapshot is in `records`
    }
    pair.found = true;
    pair.recordId = best.id;
    if (best.relation == Relation::OtherVersion) {
        pair.otherVersion = true;
        pair.explanation.append(u"another version of the same title (%1)"_s.arg(compatibilityName(best.version)));
        return pair;
    }
    pair.tier = strictExact(*other, facts, pair.explanation) ? Tier::Exact : Tier::Likely;
    pair.explanation.append(u"§9 match %1, local tier %2"_s.arg(tierName(best.tier), tierName(pair.tier)));
    return pair;
}

struct Verdict
{
    SupersedeAction action = SupersedeAction::None;
    SupersedeReason reason = SupersedeReason::NoMatch;
    QString summary;
};

// The guards an exact pair passes before anything happens to the copy at risk, in the documented order: changed →
// Recheck; busy → Defer; filed → Flag.
std::optional<Verdict> commonGuards(const SupersedeSingle &atRisk, const QString &name)
{
    if (atRisk.changedSinceIndexing) {
        return Verdict{SupersedeAction::Recheck, SupersedeReason::SingleChanged,
                       u"Recheck %1: the file changed since it was indexed"_s.arg(name)};
    }
    if (atRisk.busy) {
        return Verdict{SupersedeAction::Defer, SupersedeReason::SingleBusy,
                       u"Wait for %1: it is in use; decide again when it is idle"_s.arg(name)};
    }
    if (atRisk.place == SinglePlace::Collection) {
        return Verdict{SupersedeAction::Flag, SupersedeReason::CollectionSingle,
                       u"Review %1: it is filed in the collection, which Selecta never changes"_s.arg(name)};
    }
    return std::nullopt;
}

Verdict albumVerdict(AlbumDownloadState state, const PairMatch &match, const SupersedeSingle &single,
                     const LocalTrackFacts &albumTrack, QStringList &explanation)
{
    const QString name = display(single.track);
    if (!match.found) {
        return {SupersedeAction::None, SupersedeReason::NoMatch, u"Keep %1: the album doesn't have it"_s.arg(name)};
    }
    if (match.otherVersion) {
        return {SupersedeAction::None, SupersedeReason::VersionDiffers,
                u"Keep %1: the album has another version"_s.arg(name)};
    }
    if (match.tier != Tier::Exact) {
        return {SupersedeAction::Flag, SupersedeReason::LikelyMatch,
                u"Review %1: probably the same track as on the album"_s.arg(name)};
    }
    if (const std::optional<Verdict> guard = commonGuards(single, name)) {
        return *guard;
    }
    if (single.arrivedAfterAlbum) {
        return {SupersedeAction::Flag, SupersedeReason::ArrivedAfterAlbum,
                u"Review %1: it was downloaded after the album was already there"_s.arg(name)};
    }
    if (state != AlbumDownloadState::CompleteVerified) {
        return {SupersedeAction::Flag, SupersedeReason::AlbumIncomplete,
                u"Review %1: the album download isn't complete and verified"_s.arg(name)};
    }
    const int singleRank = qualityRank(single.track.quality);
    const int albumRank = qualityRank(albumTrack.quality);
    explanation.append(u"quality: single %1 (rank %2), album %3 (rank %4)"_s.arg(describe(single.track.quality))
                           .arg(singleRank)
                           .arg(describe(albumTrack.quality))
                           .arg(albumRank));
    if (singleRank == kQualityUnknown || albumRank == kQualityUnknown) {
        return {SupersedeAction::Flag, SupersedeReason::QualityUnknown,
                u"Review %1: its quality or the album copy's is unknown"_s.arg(name)};
    }
    if (singleRank > albumRank) {
        return {SupersedeAction::Flag, SupersedeReason::SingleBetterQuality,
                u"Review %1: the single (%2) is better than the album copy (%3)"_s.arg(
                    name, describe(single.track.quality), describe(albumTrack.quality))};
    }
    return {
        SupersedeAction::Trash, SupersedeReason::ExactMatch,
        u"Trash %1 from the Inbox: the album has the same recording (%2)"_s.arg(name, describe(albumTrack.quality))};
}

SupersedePair albumPair(AlbumDownloadState state, const PairMatch &match, const SupersedeSingle &single,
                        const LocalTrackFacts &albumTrack)
{
    SupersedePair pair;
    pair.albumTrackId = match.recordId;
    pair.singleId = single.id;
    pair.tier = match.tier;
    pair.explanation = match.explanation;
    const Verdict verdict = albumVerdict(state, match, single, albumTrack, pair.explanation);
    pair.action = verdict.action;
    pair.reason = verdict.reason;
    pair.summary = verdict.summary;
    pair.explanation.append(pair.summary);
    return pair;
}

std::shared_ptr<const MatchIndex> snapshotOf(const QList<SupersedeAlbumTrack> &tracks)
{
    MatchIndexInput input;
    for (const SupersedeAlbumTrack &track : tracks) {
        input.tracks.append(libraryTrack(track.id, track.track));
    }
    return MatchIndex::build(input);
}

} // namespace

SupersedeDecision SupersedeDecider::decide(AlbumDownloadState state, const QList<SupersedeAlbumTrack> &albumTracks,
                                           const QList<SupersedeSingle> &singles, int inboxSingleCount)
{
    SupersedeDecision decision;
    if (albumTracks.isEmpty()) {
        return decision;
    }
    const Matcher matcher(snapshotOf(albumTracks));
    QHash<quint64, const LocalTrackFacts *> records;
    for (const SupersedeAlbumTrack &track : albumTracks) {
        records.insert(track.id, &track.track);
    }
    for (const SupersedeSingle &single : singles) {
        const PairMatch match = compareLocal(matcher, single.track, records);
        if (!match.found) {
            continue; // nothing on the album: not part of the event
        }
        decision.pairs.append(albumPair(state, match, single, *records.value(match.recordId)));
        decision.trashCount += decision.pairs.constLast().action == SupersedeAction::Trash ? 1 : 0;
    }
    // Circuit breaker (note §15 rule 4, with the head implementer's floor): runaway batches need a confirmation.
    const bool tooMany = decision.trashCount > kCircuitBreakerMaxTrash;
    const bool tooLargeShare = decision.trashCount >= kCircuitBreakerShareFloor &&
                               static_cast<double>(decision.trashCount) >
                                   kCircuitBreakerMaxShare * static_cast<double>(std::max(inboxSingleCount, 0));
    decision.needsConfirmation = tooMany || tooLargeShare;
    if (tooMany) {
        decision.confirmationReason =
            u"This album would remove %1 singles from the Inbox, more than %2 at once."_s.arg(decision.trashCount)
                .arg(kCircuitBreakerMaxTrash);
    } else if (tooLargeShare) {
        decision.confirmationReason =
            u"This album would remove %1 of the %2 singles in the Inbox (more than %3 %)."_s.arg(decision.trashCount)
                .arg(inboxSingleCount)
                .arg(std::lround(kCircuitBreakerMaxShare * 100.0));
    }
    return decision;
}

SupersedePair SupersedeDecider::decidePair(AlbumDownloadState state, const SupersedeAlbumTrack &albumTrack,
                                           const SupersedeSingle &single)
{
    const Matcher matcher(snapshotOf({albumTrack}));
    const QHash<quint64, const LocalTrackFacts *> records{{albumTrack.id, &albumTrack.track}};
    PairMatch match = compareLocal(matcher, single.track, records);
    match.recordId = albumTrack.id; // also when nothing matches
    return albumPair(state, match, single, albumTrack.track);
}

SupersedeArrivalDecision SupersedeDecider::decideArrival(const SupersedeArrival &arrived,
                                                         const SupersedeSingle &existing)
{
    SupersedeArrivalDecision decision;
    decision.arrivedId = arrived.id;
    decision.existingId = existing.id;
    const Matcher matcher(snapshotOf({{existing.id, existing.track}}));
    const QHash<quint64, const LocalTrackFacts *> records{{existing.id, &existing.track}};
    const PairMatch match = compareLocal(matcher, arrived.track, records);
    decision.tier = match.tier;
    decision.explanation = match.explanation;
    const QString name = display(existing.track);
    const auto finish = [&decision](SupersedeAction action, SupersedeReason reason, const QString &summary) {
        decision.action = action;
        decision.reason = reason;
        decision.summary = summary;
        decision.explanation.append(summary);
        return decision;
    };
    if (!match.found) {
        return finish(SupersedeAction::None, SupersedeReason::NoMatch,
                      u"Keep %1: the new file isn't the same track"_s.arg(name));
    }
    if (match.otherVersion) {
        return finish(SupersedeAction::None, SupersedeReason::VersionDiffers,
                      u"Keep %1: the new file is another version"_s.arg(name));
    }
    if (match.tier != Tier::Exact) {
        return finish(SupersedeAction::Flag, SupersedeReason::LikelyMatch,
                      u"Review %1: the new file is probably the same track"_s.arg(name));
    }
    if (const std::optional<Verdict> guard = commonGuards(existing, name)) {
        return finish(guard->action, guard->reason, guard->summary);
    }
    const int arrivedRank = qualityRank(arrived.track.quality);
    const int existingRank = qualityRank(existing.track.quality);
    decision.explanation.append(
        u"quality: new %1 (rank %2), existing %3 (rank %4)"_s.arg(describe(arrived.track.quality))
            .arg(arrivedRank)
            .arg(describe(existing.track.quality))
            .arg(existingRank));
    if (arrivedRank == kQualityUnknown || existingRank == kQualityUnknown) {
        return finish(SupersedeAction::Flag, SupersedeReason::QualityUnknown,
                      u"Review %1: the quality of one copy is unknown"_s.arg(name));
    }
    if (arrivedRank > existingRank) {
        return finish(SupersedeAction::Trash, SupersedeReason::ExactMatch,
                      u"Replace %1: same recording, %2 over %3"_s.arg(name, describe(arrived.track.quality),
                                                                      describe(existing.track.quality)));
    }
    if (arrivedRank == existingRank) {
        return finish(
            SupersedeAction::Flag, SupersedeReason::SameQuality,
            u"Review %1: a second copy at the same quality (%2)"_s.arg(name, describe(arrived.track.quality)));
    }
    return finish(SupersedeAction::Flag, SupersedeReason::SingleBetterQuality,
                  u"Review %1: the copy already there (%2) is better than the new one (%3)"_s.arg(
                      name, describe(existing.track.quality), describe(arrived.track.quality)));
}

} // namespace selecta::match
