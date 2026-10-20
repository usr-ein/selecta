// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchFilter.h"

#include "core/SearchResults.h"

#include <QJsonArray>
#include <QJsonObject>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::HiddenReason;
using selecta::core::Ownership;
using selecta::core::SearchFilter;
using selecta::core::SearchFormat;
using selecta::core::SearchHit;
using selecta::core::SearchResult;

namespace {

// A hit with what peers usually send: an MP3's bitrate and length, a FLAC's length and resolution.
SearchHit hit(const QString &path, quint64 size, std::optional<quint32> bitrate, std::optional<quint32> duration,
              const QString &peer = u"vinyl_ghost"_s)
{
    SearchHit result;
    result.peer = peer;
    result.path = path.toUtf8();
    result.size = size;
    result.extension = path.mid(path.lastIndexOf(u'.') + 1).toLower();
    result.attributes.bitrate = bitrate;
    result.attributes.duration = duration;
    result.freeSlot = true;
    return result;
}

SearchHit mp3(int kbps, int seconds = 372)
{
    // The size a real file of that bitrate has (plus a little for tags).
    const auto size = (static_cast<quint64>(kbps) * 1000 / 8 * static_cast<quint64>(seconds)) + 40'000;
    return hit(u"@@music\\Okuma Drift\\Okuma Drift - Neon Rain.mp3"_s, size, static_cast<quint32>(kbps),
               static_cast<quint32>(seconds));
}

SearchHit flac(int bitDepth = 16, int sampleRate = 44'100, int seconds = 372)
{
    SearchHit result = hit(u"@@music\\Okuma Drift\\Okuma Drift - Neon Rain.flac"_s,
                           static_cast<quint64>(seconds) * 110'000, std::nullopt, static_cast<quint32>(seconds));
    result.attributes.bitDepth = static_cast<quint32>(bitDepth);
    result.attributes.sampleRate = static_cast<quint32>(sampleRate);
    return result;
}

SearchResult analysed(const SearchHit &source, const std::optional<selecta::match::ReleaseReference> &release = {},
                      const std::shared_ptr<const selecta::match::MatchIndex> &index = {})
{
    const QList<SearchResult> results = selecta::core::analyzeHits({source}, 0, index, release);
    return results.constFirst();
}

selecta::match::ReleaseReference neonRain()
{
    selecta::match::ReleaseReference reference;
    reference.artists = {u"Okuma Drift"_s};
    reference.title = u"Neon Rain"_s;
    reference.durationMs = 372'000;
    return reference;
}

} // namespace

TEST(SearchResults, ReadsNamesQualityAndFit)
{
    const SearchResult result = analysed(flac(), neonRain());
    EXPECT_TRUE(result.isAudio());
    EXPECT_EQ(result.parsed.artist.value, u"Okuma Drift"_s);
    EXPECT_EQ(result.parsed.title.value, u"Neon Rain"_s);
    EXPECT_EQ(result.folder, u"@@music\\Okuma Drift"_s);
    EXPECT_EQ(result.name, u"Okuma Drift - Neon Rain.flac"_s);
    EXPECT_EQ(result.quality.quality.codec, selecta::match::Codec::Flac);
    EXPECT_TRUE(result.quality.established);
    ASSERT_TRUE(result.fit.has_value());
    EXPECT_TRUE(result.fitsRelease());
}

TEST(SearchResults, AnotherLengthIsAnotherRecording)
{
    const SearchResult result = analysed(mp3(320, 300), neonRain());
    ASSERT_TRUE(result.fit.has_value());
    EXPECT_FALSE(result.fitsRelease());
    EXPECT_FALSE(analysed(mp3(320)).fit.has_value()); // no release: no fit at all
}

TEST(SearchResults, CompanionsAreNotTracks)
{
    const SearchResult cover = analysed(hit(u"@@music\\Okuma Drift\\cover.jpg"_s, 90'000, {}, {}), neonRain());
    EXPECT_FALSE(cover.isAudio());
    EXPECT_FALSE(cover.fit.has_value());
    EXPECT_EQ(cover.ownership, Ownership::None);
}

TEST(SearchResults, SaysWhatYouAlreadyHave)
{
    selecta::match::MatchIndexInput input;
    selecta::match::LibraryTrack owned;
    owned.id = 11;
    owned.artist = u"Okuma Drift"_s;
    owned.title = u"Neon Rain"_s;
    owned.durationMs = 372'000;
    owned.quality.codec = selecta::match::Codec::Mp3;
    owned.quality.bitrateKbps = 320;
    input.tracks.append(owned);
    selecta::match::LibraryTrack inInbox;
    inInbox.id = 5;
    inInbox.source = selecta::match::Source::Inbox;
    inInbox.artist = u"Halden Rue"_s;
    inInbox.title = u"Tidewater"_s;
    inInbox.durationMs = 301'000;
    input.tracks.append(inInbox);
    selecta::match::HistoryEntry downloaded;
    downloaded.id = 3;
    downloaded.peer = u"bassbin"_s;
    downloaded.remotePath = u"@@b\\Kassia Moreau - Night Transit.mp3"_s;
    downloaded.size = 9'000'000;
    input.history.append(downloaded);
    const auto index = selecta::match::MatchIndex::build(input);

    const SearchResult same = analysed(mp3(320), {}, index);
    EXPECT_EQ(same.ownership, Ownership::InCollection);
    EXPECT_EQ(same.ownedId, 11U);
    EXPECT_EQ(same.ownedSource, selecta::match::Source::Collection);
    EXPECT_EQ(analysed(flac(), {}, index).ownership, Ownership::Upgrade);

    const SearchResult inbox = analysed(hit(u"@@x\\Halden Rue - Tidewater.mp3"_s, 12'100'000, 320, 301), {}, index);
    EXPECT_EQ(inbox.ownership, Ownership::InInbox);
    EXPECT_EQ(inbox.ownedSource, selecta::match::Source::Inbox);

    const SearchResult before =
        analysed(hit(u"@@b\\Kassia Moreau - Night Transit.mp3"_s, 9'000'000, 320, 220, u"bassbin"_s), {}, index);
    EXPECT_EQ(before.ownership, Ownership::DownloadedBefore);
    EXPECT_EQ(before.historyIds, QList<quint64>{3});

    EXPECT_EQ(analysed(hit(u"@@x\\Someone Else - Other Song.mp3"_s, 9'000'000, 320, 220), {}, index).ownership,
              Ownership::None);
}

TEST(SearchFilterTest, ByDefaultEveryAudioFileShows)
{
    const SearchFilter filter;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(128)), false), HiddenReason::None);
    EXPECT_EQ(filter.hiddenReason(analysed(flac()), false), HiddenReason::None);
    EXPECT_EQ(filter.hiddenReason(analysed(hit(u"@@a\\cover.jpg"_s, 90'000, {}, {})), false), HiddenReason::NotAudio);
}

TEST(SearchFilterTest, OnlyThisReleaseHidesOtherRecordings)
{
    SearchFilter filter;
    const SearchResult other = analysed(mp3(320, 300), neonRain());
    EXPECT_EQ(filter.hiddenReason(other, true), HiddenReason::NotThisRelease);
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320), neonRain()), true), HiddenReason::None);
    filter.onlyThisRelease = false;
    EXPECT_EQ(filter.hiddenReason(other, true), HiddenReason::None);
    filter.onlyThisRelease = true;
    EXPECT_EQ(filter.hiddenReason(other, false), HiddenReason::None); // "Any (name only)": nothing to fit
}

TEST(SearchFilterTest, FormatsAreStrict)
{
    SearchFilter filter;
    filter.setAllowed(SearchFormat::Flac, true);
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::Format);
    EXPECT_EQ(filter.hiddenReason(analysed(flac()), false), HiddenReason::None);
    EXPECT_TRUE(filter.allows(SearchFormat::Flac));
    EXPECT_FALSE(filter.allows(SearchFormat::Other));
    filter.setAllowed(SearchFormat::Flac, false);
    EXPECT_TRUE(filter.allows(SearchFormat::Other)); // no format ticked: every format
}

TEST(SearchFilterTest, TheLossyRule)
{
    SearchFilter filter;
    filter.bitrateRule = SearchFilter::BitrateRule::Equal;
    filter.bitrateKbps = 320;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::None);
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(256)), false), HiddenReason::Bitrate);
    filter.bitrateRule = SearchFilter::BitrateRule::AtLeast;
    filter.bitrateKbps = 256;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::None);
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(192)), false), HiddenReason::Bitrate);
    filter.bitrateRule = SearchFilter::BitrateRule::AtMost;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::Bitrate);
    EXPECT_EQ(filter.hiddenReason(analysed(flac()), false), HiddenReason::None); // lossless: the other rule

    filter.bitrateRule = SearchFilter::BitrateRule::AtLeast;
    filter.bitrateKbps = 192;
    SearchHit vbr = mp3(245);
    vbr.attributes.vbr = true;
    EXPECT_EQ(filter.hiddenReason(analysed(vbr), false), HiddenReason::None);
    filter.cbrOnly = true;
    EXPECT_EQ(filter.hiddenReason(analysed(vbr), false), HiddenReason::Bitrate);
}

TEST(SearchFilterTest, TheLosslessRule)
{
    SearchFilter filter;
    filter.minBitDepth = 24;
    EXPECT_EQ(filter.hiddenReason(analysed(flac(16)), false), HiddenReason::Resolution);
    EXPECT_EQ(filter.hiddenReason(analysed(flac(24, 96'000)), false), HiddenReason::None);
    filter.minBitDepth = 16;
    filter.sampleRates = {44'100, 48'000};
    EXPECT_EQ(filter.hiddenReason(analysed(flac(24, 96'000)), false), HiddenReason::Resolution);
    EXPECT_EQ(filter.hiddenReason(analysed(flac(16, 48'000)), false), HiddenReason::None);
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(128)), false), HiddenReason::None); // lossy: the other rule
}

TEST(SearchFilterTest, StrictModeHidesWhatCantBeChecked)
{
    SearchFilter filter;
    const SearchResult silent = analysed(hit(u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 9'000'000, {}, {}));
    SearchHit liar = mp3(320);
    liar.size = quint64{128} * 1000 / 8 * 372; // declared 320, sized like 128 (SRCH-7)
    const SearchResult implausible = analysed(liar);
    EXPECT_FALSE(implausible.quality.plausible);
    EXPECT_EQ(filter.hiddenReason(silent, false), HiddenReason::None);
    EXPECT_EQ(filter.hiddenReason(implausible, false), HiddenReason::None);

    filter.strict = true;
    EXPECT_EQ(filter.hiddenReason(silent, false), HiddenReason::UnknownQuality);
    EXPECT_EQ(filter.hiddenReason(implausible, false), HiddenReason::Implausible);
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::None);

    // A rule the peer gave nothing to check against: strict hides it, otherwise it shows.
    SearchHit noDepth = flac();
    noDepth.attributes.bitDepth.reset();
    filter.minBitDepth = 24;
    EXPECT_EQ(filter.hiddenReason(analysed(noDepth), false), HiddenReason::UnknownQuality);
    filter.strict = false;
    EXPECT_EQ(filter.hiddenReason(analysed(noDepth), false), HiddenReason::None);
}

TEST(SearchFilterTest, LengthSizeAndThePeer)
{
    SearchFilter filter;
    filter.minDurationSec = 400;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::Length);
    filter.minDurationSec = 0;
    filter.maxDurationSec = 300;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::Length);
    filter.maxDurationSec = 0;
    filter.maxSizeBytes = 1'000'000;
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::Size);
    filter.maxSizeBytes = 0;

    SearchHit busy = mp3(320);
    busy.freeSlot = false;
    busy.queueLength = 40;
    filter.freeSlotOnly = true;
    EXPECT_EQ(filter.hiddenReason(analysed(busy), false), HiddenReason::NoFreeSlot);
    filter.freeSlotOnly = false;
    filter.maxQueue = 20;
    EXPECT_EQ(filter.hiddenReason(analysed(busy), false), HiddenReason::QueueTooLong);
    filter.maxQueue = 50;
    EXPECT_EQ(filter.hiddenReason(analysed(busy), false), HiddenReason::None);
}

TEST(SearchFilterTest, ExcludedWordsAndWhatYouHave)
{
    SearchFilter filter;
    filter.excludedWords = {u"  NEON "_s};
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::ExcludedWord);
    filter.excludedWords = {u"live"_s, u""_s};
    EXPECT_EQ(filter.hiddenReason(analysed(mp3(320)), false), HiddenReason::None);

    SearchResult owned = analysed(mp3(320));
    owned.ownership = Ownership::InCollection;
    SearchResult upgrade = analysed(flac());
    upgrade.ownership = Ownership::Upgrade;
    filter.owned = SearchFilter::OwnedResults::Dim;
    EXPECT_EQ(filter.hiddenReason(owned, false), HiddenReason::None);
    filter.owned = SearchFilter::OwnedResults::Hide;
    EXPECT_EQ(filter.hiddenReason(owned, false), HiddenReason::Owned);
    EXPECT_EQ(filter.hiddenReason(upgrade, false), HiddenReason::None); // an upgrade is worth seeing
}

TEST(SearchFilterTest, SelectasPresets)
{
    const QList<selecta::core::FilterPreset> presets = selecta::core::builtInPresets();
    ASSERT_EQ(presets.size(), 4);
    EXPECT_EQ(presets.constFirst().id, selecta::core::kMp3OrLosslessPreset); // the default comes first
    for (const selecta::core::FilterPreset &preset : presets) {
        EXPECT_TRUE(preset.builtIn);
        EXPECT_FALSE(preset.name.isEmpty());
        EXPECT_EQ(selecta::core::builtInPreset(preset.id), preset);
    }
    EXPECT_FALSE(selecta::core::builtInPreset(u"user-1").has_value());

    const SearchFilter mp3OrLossless = selecta::core::builtInPreset(selecta::core::kMp3OrLosslessPreset)->filter;
    EXPECT_EQ(mp3OrLossless.hiddenReason(analysed(mp3(320)), false), HiddenReason::None);
    EXPECT_EQ(mp3OrLossless.hiddenReason(analysed(flac()), false), HiddenReason::None);
    EXPECT_EQ(mp3OrLossless.hiddenReason(analysed(mp3(256)), false), HiddenReason::Bitrate);
    SearchHit aac = mp3(320);
    aac.path = "@@a\\Okuma Drift - Neon Rain.m4a";
    aac.extension = u"m4a"_s;
    EXPECT_EQ(mp3OrLossless.hiddenReason(analysed(aac), false), HiddenReason::Format);

    const SearchFilter lossless = selecta::core::builtInPreset(selecta::core::kLosslessPreset)->filter;
    EXPECT_EQ(lossless.hiddenReason(analysed(mp3(320)), false), HiddenReason::Format);
    EXPECT_EQ(lossless.hiddenReason(analysed(flac()), false), HiddenReason::None);

    const SearchFilter hiRes = selecta::core::builtInPreset(selecta::core::kHiResPreset)->filter;
    EXPECT_EQ(hiRes.hiddenReason(analysed(mp3(320)), false), HiddenReason::Format);
    EXPECT_EQ(hiRes.hiddenReason(analysed(flac(16)), false), HiddenReason::Resolution);
    EXPECT_EQ(hiRes.hiddenReason(analysed(flac(24, 96'000)), false), HiddenReason::None);

    const SearchFilter any = selecta::core::builtInPreset(selecta::core::kAnyQualityPreset)->filter;
    EXPECT_EQ(any.hiddenReason(analysed(mp3(128)), false), HiddenReason::None);
    EXPECT_EQ(any, SearchFilter());
}

TEST(SearchFilterTest, SlowPeersCanBeLeftOut)
{
    SearchFilter filter;
    filter.minSpeedKBps = 100;
    SearchHit slow = mp3(320);
    slow.averageSpeed = 99'999;
    EXPECT_EQ(filter.hiddenReason(analysed(slow), false), HiddenReason::TooSlow);
    SearchHit fast = mp3(320);
    fast.averageSpeed = 100'000;
    EXPECT_EQ(filter.hiddenReason(analysed(fast), false), HiddenReason::None);
    EXPECT_FALSE(selecta::core::hiddenReasonLabel(HiddenReason::TooSlow).isEmpty());
}

TEST(SearchFilterTest, FiltersRoundTripAsJsonAndBadValuesReadAsDefaults)
{
    SearchFilter filter;
    filter.setAllowed(selecta::core::SearchFormat::Flac, true);
    filter.setAllowed(selecta::core::SearchFormat::Mp3, true);
    filter.bitrateRule = SearchFilter::BitrateRule::Equal;
    filter.bitrateKbps = 256;
    filter.cbrOnly = true;
    filter.minBitDepth = 24;
    filter.sampleRates = {44'100, 96'000};
    filter.minDurationSec = 120;
    filter.maxDurationSec = 900;
    filter.minSizeBytes = 1'000'000;
    filter.maxSizeBytes = 200'000'000;
    filter.freeSlotOnly = true;
    filter.maxQueue = 20;
    filter.minSpeedKBps = 250;
    filter.excludedWords = {u"live"_s, u"karaoke"_s};
    filter.strict = true;
    filter.owned = SearchFilter::OwnedResults::Hide;
    filter.onlyThisRelease = false;
    EXPECT_EQ(selecta::core::filterFromJson(selecta::core::filterToJson(filter)), filter);

    // Missing, mistyped and out-of-range values read as the defaults; lists stay bounded.
    QJsonObject bad;
    bad.insert(u"bitrateRule"_s, 99);
    bad.insert(u"bitrateKbps"_s, u"lots"_s);
    bad.insert(u"minBitDepth"_s, -4);
    bad.insert(u"sampleRates"_s, QJsonArray{44'100, -1, 9'999'999, 44'100});
    QJsonArray words;
    for (int i = 0; i < 500; ++i) {
        words.append(u"word%1"_s.arg(i));
    }
    bad.insert(u"excludedWords"_s, words);
    const SearchFilter read = selecta::core::filterFromJson(bad);
    EXPECT_EQ(read.bitrateRule, SearchFilter::BitrateRule::Any);
    EXPECT_EQ(read.bitrateKbps, SearchFilter().bitrateKbps);
    EXPECT_EQ(read.minBitDepth, 0);
    EXPECT_EQ(read.sampleRates, QList<int>{44'100});
    EXPECT_EQ(read.excludedWords.size(), 50);
    EXPECT_TRUE(read.onlyThisRelease);
    EXPECT_EQ(selecta::core::filterFromJson(QJsonObject()), SearchFilter());
}

TEST(SearchFilterTest, CountsSayWhyResultsAreHidden)
{
    selecta::core::FilterCounts counts;
    counts.count(HiddenReason::None);
    counts.count(HiddenReason::Format);
    counts.count(HiddenReason::Format);
    counts.count(HiddenReason::Owned);
    EXPECT_EQ(counts.total, 4);
    EXPECT_EQ(counts.shown, 1);
    EXPECT_EQ(counts.hidden.at(static_cast<std::size_t>(HiddenReason::Format)), 2);
    EXPECT_EQ(counts.hidden.at(static_cast<std::size_t>(HiddenReason::Owned)), 1);
    EXPECT_FALSE(selecta::core::hiddenReasonLabel(HiddenReason::Implausible).isEmpty());
}
