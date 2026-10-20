// SPDX-License-Identifier: GPL-3.0-or-later
// The supersede decision table (note §15, T36–T42) and the arrival pipeline's replacement decision.
#include "match/Supersede.h"

#include "MatchTestSupport.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::AlbumDownloadState;
using selecta::match::AudioQuality;
using selecta::match::Codec;
using selecta::match::LocalTrackFacts;
using selecta::match::SinglePlace;
using selecta::match::SupersedeAction;
using selecta::match::SupersedeAlbumTrack;
using selecta::match::SupersedeArrivalDecision;
using selecta::match::SupersedeDecider;
using selecta::match::SupersedeDecision;
using selecta::match::SupersedePair;
using selecta::match::SupersedeReason;
using selecta::match::SupersedeSingle;
using selecta::match::Tier;

namespace {

AudioQuality mp3(int kbps)
{
    return {Codec::Mp3, kbps, false, 44100, 0};
}

AudioQuality flac(int bits = 16)
{
    return {Codec::Flac, 0, std::nullopt, 44100, bits};
}

LocalTrackFacts facts(const QString &artist, const QString &title, qint64 ms, const AudioQuality &quality = mp3(320))
{
    return {artist, title, ms, quality};
}

SupersedeAlbumTrack albumTrack(quint64 id, const LocalTrackFacts &track)
{
    return {id, track};
}

SupersedeSingle single(quint64 id, const LocalTrackFacts &track, SinglePlace place = SinglePlace::Inbox)
{
    SupersedeSingle single;
    single.id = id;
    single.track = track;
    single.place = place;
    return single;
}

LocalTrackFacts glue()
{
    return {u"Bicep"_s, u"Glue"_s, 269'400, {Codec::Mp3, 320, false, 44100, 0}};
}

SupersedePair decide(AlbumDownloadState state, const LocalTrackFacts &album, const SupersedeSingle &s)
{
    return SupersedeDecider::decidePair(state, albumTrack(1, album), s);
}

TEST(Supersede, AnEqualInboxSingleIsTrashed) // T36
{
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, glue(), single(9, glue()));
    EXPECT_EQ(pair.tier, Tier::Exact);
    EXPECT_EQ(pair.action, SupersedeAction::Trash);
    EXPECT_EQ(pair.reason, SupersedeReason::ExactMatch);
    EXPECT_EQ(pair.albumTrackId, 1U);
    EXPECT_EQ(pair.singleId, 9U);
    EXPECT_FALSE(pair.summary.isEmpty());
    EXPECT_FALSE(pair.explanation.isEmpty());
}

TEST(Supersede, ABetterSingleIsOnlyFlagged) // T37
{
    LocalTrackFacts album = glue();
    album.quality = mp3(192);
    LocalTrackFacts lossless = glue();
    lossless.quality = flac();
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, album, single(9, lossless));
    EXPECT_EQ(pair.action, SupersedeAction::Flag);
    EXPECT_EQ(pair.reason, SupersedeReason::SingleBetterQuality);
}

TEST(Supersede, AFiledSingleIsNeverTouched) // T38
{
    const SupersedePair pair =
        decide(AlbumDownloadState::CompleteVerified, glue(), single(9, glue(), SinglePlace::Collection));
    EXPECT_EQ(pair.action, SupersedeAction::Flag);
    EXPECT_EQ(pair.reason, SupersedeReason::CollectionSingle);
}

TEST(Supersede, AnIncompleteAlbumOnlyFlags) // T39
{
    for (const AlbumDownloadState state : {AlbumDownloadState::Partial, AlbumDownloadState::Failed,
                                           AlbumDownloadState::Cancelled, AlbumDownloadState::InProgress}) {
        const SupersedePair pair = decide(state, glue(), single(9, glue()));
        EXPECT_EQ(pair.tier, Tier::Exact);
        EXPECT_EQ(pair.action, SupersedeAction::Flag);
        EXPECT_EQ(pair.reason, SupersedeReason::AlbumIncomplete);
    }
}

TEST(Supersede, AnotherVersionIsNotSuperseded) // T40
{
    const LocalTrackFacts original = facts(u"Bicep"_s, u"Glue (Original Mix)"_s, 269'400);
    const LocalTrackFacts radio = facts(u"Bicep"_s, u"Glue (Radio Edit)"_s, 199'000);
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, original, single(9, radio));
    EXPECT_EQ(pair.tier, Tier::None);
    EXPECT_EQ(pair.action, SupersedeAction::None);
    EXPECT_EQ(pair.reason, SupersedeReason::VersionDiffers);
}

TEST(Supersede, AChangedSingleIsRechecked) // T41
{
    SupersedeSingle changed = single(9, glue());
    changed.changedSinceIndexing = true;
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, glue(), changed);
    EXPECT_EQ(pair.action, SupersedeAction::Recheck);
    EXPECT_EQ(pair.reason, SupersedeReason::SingleChanged);
}

TEST(Supersede, ABusySingleIsDeferred)
{
    SupersedeSingle busy = single(9, glue());
    busy.busy = true;
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, glue(), busy);
    EXPECT_EQ(pair.action, SupersedeAction::Defer);
    EXPECT_EQ(pair.reason, SupersedeReason::SingleBusy);
}

TEST(Supersede, GuardsComeInTheDocumentedOrder)
{
    // Changed beats busy beats filed beats arrived-after beats incomplete.
    SupersedeSingle all = single(9, glue(), SinglePlace::Collection);
    all.changedSinceIndexing = true;
    all.busy = true;
    all.arrivedAfterAlbum = true;
    EXPECT_EQ(decide(AlbumDownloadState::Partial, glue(), all).reason, SupersedeReason::SingleChanged);
    all.changedSinceIndexing = false;
    EXPECT_EQ(decide(AlbumDownloadState::Partial, glue(), all).reason, SupersedeReason::SingleBusy);
    all.busy = false;
    EXPECT_EQ(decide(AlbumDownloadState::Partial, glue(), all).reason, SupersedeReason::CollectionSingle);
    all.place = SinglePlace::Inbox;
    EXPECT_EQ(decide(AlbumDownloadState::Partial, glue(), all).reason, SupersedeReason::ArrivedAfterAlbum);
    all.arrivedAfterAlbum = false;
    EXPECT_EQ(decide(AlbumDownloadState::Partial, glue(), all).reason, SupersedeReason::AlbumIncomplete);
}

TEST(Supersede, UnknownQualityOnlyFlags)
{
    LocalTrackFacts unknown = glue();
    unknown.quality = {Codec::Mp3, 0, std::nullopt, 0, 0}; // a lossy file without a bitrate
    EXPECT_EQ(decide(AlbumDownloadState::CompleteVerified, glue(), single(9, unknown)).reason,
              SupersedeReason::QualityUnknown);
    EXPECT_EQ(decide(AlbumDownloadState::CompleteVerified, unknown, single(9, glue())).reason,
              SupersedeReason::QualityUnknown);
}

TEST(Supersede, ExactNeedsLengthsWithinTwoSeconds)
{
    LocalTrackFacts longer = glue();
    longer.durationMs += 2'600; // inside the matcher's exact window (3 s), outside the local rule (2 s)
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, glue(), single(9, longer));
    EXPECT_EQ(pair.tier, Tier::Likely);
    EXPECT_EQ(pair.action, SupersedeAction::Flag);
    EXPECT_EQ(pair.reason, SupersedeReason::LikelyMatch);
}

TEST(Supersede, ExactNeedsEqualArtistSets)
{
    const LocalTrackFacts collaboration = facts(u"Bicep & Clara La San"_s, u"Glue"_s, 269'400);
    const SupersedePair pair = decide(AlbumDownloadState::CompleteVerified, glue(), single(9, collaboration));
    EXPECT_NE(pair.action, SupersedeAction::Trash);
}

TEST(Supersede, UnrelatedSinglesAreLeftOut)
{
    const QList<SupersedeAlbumTrack> album{albumTrack(1, glue()),
                                           albumTrack(2, facts(u"Bicep"_s, u"Atlas"_s, 290'100))};
    const QList<SupersedeSingle> singles{single(7, facts(u"Daft Punk"_s, u"Aerodynamic"_s, 212'000)),
                                         single(8, glue())};
    const SupersedeDecision decision =
        SupersedeDecider::decide(AlbumDownloadState::CompleteVerified, album, singles, 2);
    ASSERT_EQ(decision.pairs.size(), 1);
    EXPECT_EQ(decision.pairs.front().singleId, 8U);
    EXPECT_EQ(decision.pairs.front().albumTrackId, 1U);
    EXPECT_EQ(decision.trashCount, 1);
    EXPECT_FALSE(decision.needsConfirmation);

    const SupersedePair none = decide(AlbumDownloadState::CompleteVerified, glue(),
                                      single(7, facts(u"Daft Punk"_s, u"Aerodynamic"_s, 212'000)));
    EXPECT_EQ(none.action, SupersedeAction::None);
    EXPECT_EQ(none.reason, SupersedeReason::NoMatch);
}

// An album event in which `trashes` singles would go, with `inbox` singles in the Inbox.
SupersedeDecision event(int trashes, int inbox)
{
    QList<SupersedeAlbumTrack> album;
    QList<SupersedeSingle> singles;
    for (int i = 0; i < trashes; ++i) {
        const LocalTrackFacts track =
            facts(u"Artist %1"_s.arg(i), u"Song number %1"_s.arg(i), 200'000 + (static_cast<qint64>(i) * 9'000));
        album.append(albumTrack(static_cast<quint64>(i) + 1, track));
        singles.append(single(static_cast<quint64>(i) + 100, track));
    }
    return SupersedeDecider::decide(AlbumDownloadState::CompleteVerified, album, singles, inbox);
}

TEST(Supersede, CircuitBreakerHasAFloor) // the head implementer's decision (Q-M2)
{
    const SupersedeDecision three = event(3, 5); // 60 % of the Inbox, but only 3 files
    EXPECT_EQ(three.trashCount, 3);
    EXPECT_FALSE(three.needsConfirmation);

    const SupersedeDecision four = event(4, 10); // 4 files, 40 %
    EXPECT_EQ(four.trashCount, 4);
    EXPECT_TRUE(four.needsConfirmation);
    EXPECT_FALSE(four.confirmationReason.isEmpty());

    const SupersedeDecision small = event(4, 20); // 4 files, 20 %
    EXPECT_FALSE(small.needsConfirmation);

    const SupersedeDecision eleven = event(11, 100); // more than 10
    EXPECT_EQ(eleven.trashCount, 11);
    EXPECT_TRUE(eleven.needsConfirmation);
}

TEST(Supersede, AVariousArtistsAlbumTakingTwelveSinglesAsksFirst) // T42
{
    const SupersedeDecision decision = event(12, 400);
    EXPECT_EQ(decision.trashCount, 12);
    EXPECT_TRUE(decision.needsConfirmation);
    for (const SupersedePair &pair : decision.pairs) {
        EXPECT_EQ(pair.action, SupersedeAction::Trash); // they stay Trash: core asks before acting
    }
}

// --- The arrival pipeline --------------------------------------------------------------------------------------------

SupersedeArrivalDecision arrive(const AudioQuality &arrived, const AudioQuality &existing,
                                SinglePlace place = SinglePlace::Inbox)
{
    LocalTrackFacts newCopy = glue();
    newCopy.quality = arrived;
    LocalTrackFacts oldCopy = glue();
    oldCopy.quality = existing;
    return SupersedeDecider::decideArrival({42, newCopy}, single(9, oldCopy, place));
}

TEST(SupersedeArrival, ABetterCopyReplacesTheOldOne)
{
    const SupersedeArrivalDecision decision = arrive(flac(), mp3(192));
    EXPECT_EQ(decision.arrivedId, 42U);
    EXPECT_EQ(decision.existingId, 9U);
    EXPECT_EQ(decision.tier, Tier::Exact);
    EXPECT_EQ(decision.action, SupersedeAction::Trash);
    EXPECT_EQ(decision.reason, SupersedeReason::ExactMatch);
    EXPECT_TRUE(decision.summary.contains(u"FLAC 16-bit"_s)) << decision.summary.toStdString();
    EXPECT_TRUE(decision.summary.contains(u"MP3 192"_s)) << decision.summary.toStdString();
}

TEST(SupersedeArrival, TheSameQualityIsADuplicate)
{
    const SupersedeArrivalDecision decision = arrive(mp3(320), mp3(320));
    EXPECT_EQ(decision.action, SupersedeAction::Flag);
    EXPECT_EQ(decision.reason, SupersedeReason::SameQuality);
}

TEST(SupersedeArrival, AWorseCopyNeverReplaces)
{
    const SupersedeArrivalDecision decision = arrive(mp3(192), flac(24));
    EXPECT_EQ(decision.action, SupersedeAction::Flag);
    EXPECT_EQ(decision.reason, SupersedeReason::SingleBetterQuality);
}

TEST(SupersedeArrival, TheSourceQualityIsRankedAsGiven)
{
    // Both files are MP3 320 on disk (PRD D29); core passes the source quality: the FLAC-sourced copy is better.
    EXPECT_EQ(arrive(flac(), mp3(320)).action, SupersedeAction::Trash);
}

TEST(SupersedeArrival, FiledCopiesAreNeverReplaced)
{
    const SupersedeArrivalDecision decision = arrive(flac(), mp3(192), SinglePlace::Collection);
    EXPECT_EQ(decision.action, SupersedeAction::Flag);
    EXPECT_EQ(decision.reason, SupersedeReason::CollectionSingle);
}

TEST(SupersedeArrival, BusyAndChangedCopiesWait)
{
    LocalTrackFacts oldCopy = glue();
    oldCopy.quality = mp3(192);
    LocalTrackFacts newCopy = glue();
    newCopy.quality = flac();
    SupersedeSingle existing = single(9, oldCopy);
    existing.busy = true;
    EXPECT_EQ(SupersedeDecider::decideArrival({42, newCopy}, existing).action, SupersedeAction::Defer);
    existing.changedSinceIndexing = true;
    EXPECT_EQ(SupersedeDecider::decideArrival({42, newCopy}, existing).action, SupersedeAction::Recheck);
}

TEST(SupersedeArrival, AnotherVersionOrAnotherTrackIsLeftAlone)
{
    const LocalTrackFacts radio = facts(u"Bicep"_s, u"Glue (Radio Edit)"_s, 199'000, flac());
    const SupersedeArrivalDecision version = SupersedeDecider::decideArrival({42, radio}, single(9, glue()));
    EXPECT_EQ(version.action, SupersedeAction::None);
    EXPECT_EQ(version.reason, SupersedeReason::VersionDiffers);

    const LocalTrackFacts other = facts(u"Daft Punk"_s, u"Aerodynamic"_s, 212'000, flac());
    const SupersedeArrivalDecision unrelated = SupersedeDecider::decideArrival({42, other}, single(9, glue()));
    EXPECT_EQ(unrelated.action, SupersedeAction::None);
    EXPECT_EQ(unrelated.reason, SupersedeReason::NoMatch);
}

TEST(SupersedeArrival, ALikelyMatchOnlyFlags)
{
    LocalTrackFacts typo = glue();
    typo.title = u"Glue (Original Mix)"_s; // Unspecified against Original: the same work, not provably the same
    typo.quality = flac();
    typo.durationMs += 2'500;
    const SupersedeArrivalDecision decision = SupersedeDecider::decideArrival({42, typo}, single(9, glue()));
    EXPECT_NE(decision.action, SupersedeAction::Trash);
}

} // namespace
