// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/ReleaseResolver.h"

#include "meta/DeezerClient.h"
#include "meta/MetaCache.h"
#include "meta/SpotifyAuth.h"
#include "meta/SpotifyClient.h"
#include "meta/TokenStore.h"
#include "meta/detail/TitleInfo.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"
#include "meta/support/Fixtures.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
using selecta::base::ManualClock;
using selecta::match::VersionCompatibility;
using selecta::meta::testing::awaitAdvancing;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::fixture;
using selecta::meta::testing::header;
using selecta::meta::testing::spinUntil;

namespace {

Release release(const QString &title, const QStringList &artists, std::optional<qint64> durationMs,
                const QString &isrc = {}, Source source = Source::Spotify)
{
    Release r;
    r.source = source;
    r.title = title;
    r.artists = artists;
    r.durationMs = durationMs;
    r.isrc = isrc;
    r.album = u"Glass Harbour"_s;
    if (source == Source::Spotify) {
        r.spotifyId = u"sp-"_s + isrc;
    } else {
        r.deezerId = u"dz-"_s + isrc;
    }
    return r;
}

ResolveQuery line(const QString &artist, const QString &title, std::optional<qint64> durationMs = std::nullopt)
{
    ResolveQuery query;
    query.artist = artist;
    query.title = title;
    query.durationMs = durationMs;
    return query;
}

// A small catalogue with the traps of the note §4.4 and §14.5.
const QString kKestrel = u"Kestrel Moon"_s;
Release extendedMix()
{
    return release(u"Glass Harbour - Extended Mix"_s, {kKestrel}, 391000, u"GBXYZ2600101"_s);
}
Release radioEdit()
{
    return release(u"Glass Harbour - Radio Edit"_s, {kKestrel}, 201500, u"GBXYZ2600102"_s);
}
Release nightTramRemix()
{
    return release(u"Glass Harbour - Night Tram Remix"_s, {kKestrel, u"Night Tram"_s}, 412000, u"GBXYZ2600103"_s);
}
Release originalMix()
{
    return release(u"Glass Harbour - Original Mix"_s, {kKestrel}, 362000, u"GBXYZ2600104"_s);
}
Release mixedSegment()
{
    return release(u"Glass Harbour (Mixed)"_s, {kKestrel}, 175000, u"GBXYZ2600190"_s, Source::Deezer);
}
Release slowedReverb()
{
    return release(u"Glass Harbour (Slowed + Reverb)"_s, {u"Lofi Uploads"_s}, 455000, u"QZXXX2600001"_s);
}
Release namesake()
{
    return release(u"Glass Harbour"_s, {u"Other Band"_s}, 300000, u"USABC2600001"_s);
}

QString best(const Resolution &resolution)
{
    return resolution.candidates.isEmpty() ? QString() : resolution.candidates.first().release.title;
}

} // namespace

// --- The labelled table of tricky lines (contract M4) ---------------------------------------------------------------

TEST(ReleaseResolverTable, ExactVersionWinsConfidently)
{
    const Resolution r = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour (Extended Mix)"_s),
                                               {radioEdit(), extendedMix(), nightTramRemix()});
    EXPECT_EQ(best(r), u"Glass Harbour - Extended Mix"_s);
    EXPECT_EQ(r.confidence, Confidence::Confident);
    EXPECT_EQ(r.candidates.first().version, VersionCompatibility::Same);
    // The other versions are kept, last, with score 0.
    EXPECT_EQ(r.candidates.size(), 3);
    EXPECT_DOUBLE_EQ(r.candidates.at(1).score, 0.0);
    EXPECT_EQ(r.candidates.at(1).version, VersionCompatibility::OtherVersion);
}

TEST(ReleaseResolverTable, RemixAgainstOriginal)
{
    const Resolution r = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour (Night Tram Remix)"_s),
                                               {originalMix(), extendedMix(), nightTramRemix()});
    EXPECT_EQ(best(r), u"Glass Harbour - Night Tram Remix"_s);
    EXPECT_EQ(r.confidence, Confidence::Confident); // the remixer explains the second credited artist
    const Resolution other =
        ReleaseResolver::rank(line(kKestrel, u"Glass Harbour (Bicep Remix)"_s), {nightTramRemix()});
    EXPECT_EQ(other.confidence, Confidence::None); // a remix by someone else is another version
}

TEST(ReleaseResolverTable, UnspecifiedLinePrefersTheOriginal)
{
    const Resolution r =
        ReleaseResolver::rank(line(kKestrel, u"Glass Harbour"_s), {extendedMix(), radioEdit(), originalMix()});
    EXPECT_EQ(best(r), u"Glass Harbour - Original Mix"_s);
    EXPECT_EQ(r.candidates.first().version, VersionCompatibility::ProbablySame);
    EXPECT_GE(r.confidence, Confidence::Likely);
    // Without an original, two versions tie: chosen for a glance, never confidently…
    const Resolution tie = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour"_s), {extendedMix(), radioEdit()});
    EXPECT_EQ(tie.confidence, Confidence::Likely);
    // …even when both sources carry both versions.
    Release deezerExtended = extendedMix();
    deezerExtended.source = Source::Deezer;
    deezerExtended.title = u"Glass Harbour (Extended Mix)"_s;
    Release deezerRadio = radioEdit();
    deezerRadio.source = Source::Deezer;
    deezerRadio.title = u"Glass Harbour (Radio Edit)"_s;
    const Resolution both = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour"_s),
                                                  {extendedMix(), radioEdit(), deezerExtended, deezerRadio});
    EXPECT_EQ(both.candidates.size(), 2);
    EXPECT_EQ(both.confidence, Confidence::Likely);
}

TEST(ReleaseResolverTable, TheLengthTellsRadioFromExtended)
{
    const Resolution r = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour"_s, 201000),
                                               {extendedMix(), radioEdit(), nightTramRemix()});
    EXPECT_EQ(best(r), u"Glass Harbour - Radio Edit"_s);
    EXPECT_EQ(r.confidence, Confidence::Confident);
    EXPECT_TRUE(r.candidates.first().reasons.first().contains(u"length Δ 1 s"_s));
}

TEST(ReleaseResolverTable, RadioEditLineAgainstExtendedOnly)
{
    const Resolution r = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour (Radio Edit)"_s), {extendedMix()});
    EXPECT_EQ(r.confidence, Confidence::None);
    EXPECT_EQ(r.candidates.first().version, VersionCompatibility::OtherVersion);
}

TEST(ReleaseResolverTable, FeaturedArtistsOnEitherSide)
{
    const Release spotifyStyle = release(u"Night Bus"_s, {kKestrel, u"Clara Vale"_s}, 280000, u"GBXYZ2600201"_s);
    const Release deezerStyle =
        release(u"Night Bus (feat. Clara Vale)"_s, {kKestrel}, 280000, u"GBXYZ2600202"_s, Source::Deezer);
    const Release soloVersion = release(u"Night Bus"_s, {kKestrel}, 280000, u"GBXYZ2600203"_s);
    const ResolveQuery query = line(u"Kestrel Moon feat. Clara Vale"_s, u"Night Bus"_s);
    EXPECT_GE(ReleaseResolver::score(query, spotifyStyle).score, 0.92);
    EXPECT_GE(ReleaseResolver::score(query, deezerStyle).score, 0.92);
    EXPECT_LT(ReleaseResolver::score(query, soloVersion).score, 0.92); // the featured artist is missing
    // A line without the featured artist still finds the Spotify credit (the extra artist costs a little).
    EXPECT_GE(ReleaseResolver::score(line(kKestrel, u"Night Bus"_s), spotifyStyle).score, 0.85);
}

TEST(ReleaseResolverTable, TyposAreTolerated)
{
    const Resolution r = ReleaseResolver::rank(line(u"Kestrl Moon"_s, u"Glass Harbor (Extended Mix)"_s),
                                               {extendedMix(), radioEdit(), namesake()});
    EXPECT_EQ(best(r), u"Glass Harbour - Extended Mix"_s);
    EXPECT_GE(r.confidence, Confidence::Likely);
    EXPECT_LT(r.candidates.first().score, 1.0); // typos never look exact
}

TEST(ReleaseResolverTable, DjMixSegmentsAreDemoted)
{
    const Resolution r = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour"_s), {mixedSegment(), extendedMix()});
    EXPECT_EQ(best(r), u"Glass Harbour - Extended Mix"_s);
    const Resolution onlyMix = ReleaseResolver::rank(line(kKestrel, u"Glass Harbour"_s), {mixedSegment()});
    EXPECT_EQ(onlyMix.confidence, Confidence::None);
    EXPECT_TRUE(onlyMix.candidates.first().reasons.join(u" "_s).contains(u"DJ-mix"_s));
    // Asking for the mix is fine.
    EXPECT_GE(ReleaseResolver::score(line(kKestrel, u"Glass Harbour (Mixed)"_s), mixedSegment()).score, 0.9);
}

TEST(ReleaseResolverTable, DerivativeJunkIsDemotedUnlessAskedFor)
{
    const Candidate junk = ReleaseResolver::score(line(kKestrel, u"Glass Harbour"_s), slowedReverb());
    EXPECT_LT(junk.score, 0.2);
    EXPECT_TRUE(junk.reasons.join(u" "_s).contains(u"derivative"_s));
    const Candidate asked =
        ReleaseResolver::score(line(u"Lofi Uploads"_s, u"Glass Harbour (Slowed + Reverb)"_s), slowedReverb());
    EXPECT_GE(asked.score, 0.9);
}

TEST(ReleaseResolverTable, SameTitleByAnotherArtistIsHalved)
{
    const Candidate cover = ReleaseResolver::score(line(kKestrel, u"Glass Harbour"_s), namesake());
    EXPECT_LT(cover.score, 0.6);
    EXPECT_TRUE(cover.reasons.join(u" "_s).contains(u"another artist"_s));
}

TEST(ReleaseResolverTable, IsrcMakesItExact)
{
    ResolveQuery query = line(kKestrel, u"Glass Harbour"_s);
    query.isrc = u"gb-xyz-26-00101"_s;
    const Resolution r = ReleaseResolver::rank(query, {radioEdit(), extendedMix()});
    EXPECT_EQ(best(r), u"Glass Harbour - Extended Mix"_s);
    EXPECT_EQ(r.confidence, Confidence::Exact);
    // An ISRC whose length disagrees is no proof.
    query.durationMs = 300000;
    EXPECT_NE(ReleaseResolver::rank(query, {extendedMix()}).confidence, Confidence::Exact);
    // Nor one on a completely different title.
    ResolveQuery wrongTitle = line(kKestrel, u"Something Else Entirely"_s);
    wrongTitle.isrc = u"GBXYZ2600101"_s;
    EXPECT_NE(ReleaseResolver::rank(wrongTitle, {extendedMix()}).confidence, Confidence::Exact);
}

TEST(ReleaseResolverTable, OriginalMixLineAgainstAnUnspecifiedRelease)
{
    const Release plain = release(u"Glass Harbour"_s, {kKestrel}, 362000, u"GBXYZ2600105"_s);
    const Candidate c = ReleaseResolver::score(line(kKestrel, u"Glass Harbour (Original Mix)"_s), plain);
    EXPECT_EQ(c.version, VersionCompatibility::ProbablySame);
    EXPECT_GE(c.score, 0.92);
}

TEST(ReleaseResolverTable, LabelAndAlbumHintsCount)
{
    Release labelled = extendedMix();
    labelled.label = u"Lantern Grove Records"_s;
    ResolveQuery query = line(kKestrel, u"Glass Harbour (Extended Mix)"_s);
    query.label = u"lantern grove records"_s;
    query.album = u"Glass Harbour"_s;
    const Candidate c = ReleaseResolver::score(query, labelled);
    EXPECT_TRUE(c.reasons.first().contains(u"label agrees"_s));
    EXPECT_TRUE(c.reasons.first().contains(u"album 1.00"_s));
    query.label = u"Other Imprint"_s;
    EXPECT_LT(ReleaseResolver::score(query, labelled).score, c.score);
}

TEST(ReleaseResolverTable, DuplicatesAcrossSourcesMergeAndAgreementRaisesTheTier)
{
    const Release spotify = extendedMix();
    Release deezer = release(u"Glass Harbour (Extended Mix)"_s, {kKestrel}, 391000, u"GBXYZ2600101"_s, Source::Deezer);
    deezer.deezerUrl = QUrl(u"https://www.deezer.com/track/3135553"_s);
    deezer.label = u"Lantern Grove Records"_s;
    deezer.labelSource = Source::Deezer;
    // A typo keeps the score below Confident on its own…
    const ResolveQuery query = line(u"Kestrl Moon"_s, u"Glass Harbor (Extended Mix)"_s);
    const Resolution alone = ReleaseResolver::rank(query, {spotify});
    const Resolution both = ReleaseResolver::rank(query, {spotify, deezer});
    ASSERT_EQ(both.candidates.size(), 1);
    const Release &merged = both.candidates.first().release;
    EXPECT_EQ(merged.source, Source::Spotify);
    EXPECT_FALSE(merged.spotifyId.isEmpty());
    EXPECT_EQ(merged.deezerId, deezer.deezerId);
    EXPECT_EQ(merged.label, u"Lantern Grove Records"_s);
    EXPECT_EQ(merged.labelSource, Source::Deezer);
    // …and two sources agreeing on the ISRC raise it one tier.
    if (alone.confidence == Confidence::Likely) {
        EXPECT_EQ(both.confidence, Confidence::Confident);
        EXPECT_TRUE(both.candidates.first().reasons.join(u" "_s).contains(u"two sources"_s));
    }
}

TEST(ReleaseResolverTable, AMergedCandidateShowsTheOriginalAlbumNotACompilation)
{
    Release compilation = extendedMix();
    compilation.album = u"Club Essentials 2026"_s;
    compilation.albumType = u"compilation"_s;
    compilation.spotifyId = u"compilationCopy"_s;
    Release single = extendedMix();
    single.albumType = u"single"_s;
    single.spotifyId = u"singleCopy"_s;
    Release deezer = extendedMix();
    deezer.source = Source::Deezer;
    deezer.spotifyId.clear();
    deezer.deezerId = u"3135553"_s;
    const Resolution r =
        ReleaseResolver::rank(line(kKestrel, u"Glass Harbour (Extended Mix)"_s), {deezer, compilation, single});
    ASSERT_EQ(r.candidates.size(), 1);
    const Release &chosen = r.candidates.first().release;
    EXPECT_EQ(chosen.source, Source::Spotify);
    EXPECT_EQ(chosen.spotifyId, u"singleCopy"_s);
    EXPECT_EQ(chosen.album, u"Glass Harbour"_s);
    EXPECT_EQ(chosen.deezerId, u"3135553"_s);
}

TEST(ReleaseResolverTable, FreeTextLinesWithoutAnArtist)
{
    const Resolution r = ReleaseResolver::rank(line(QString(), u"Glass Harbour Extended Mix"_s), {extendedMix()});
    EXPECT_FALSE(r.candidates.isEmpty());
    EXPECT_GE(r.candidates.first().score, 0.6);
}

TEST(TitleInfo, JunkMixesAndPrimaryArtists)
{
    EXPECT_EQ(detail::analyseTitle(u"Glass Harbour (Slowed + Reverb)"_s).junk, (QStringList{u"slowed"_s, u"reverb"_s}));
    EXPECT_EQ(detail::analyseTitle(u"Glass Harbour (Sped Up)"_s).junk, QStringList{u"sped up"_s});
    EXPECT_TRUE(detail::analyseTitle(u"Cover Me"_s).junk.isEmpty()); // "cover" in the title itself isn't junk
    EXPECT_TRUE(detail::analyseTitle(u"Glass Harbour (Mixed)"_s).djMix);
    EXPECT_TRUE(detail::analyseTitle(u"Glass Harbour - Mixed"_s).djMix);
    EXPECT_FALSE(detail::analyseTitle(u"Glass Harbour (Extended Mix)"_s).djMix);
    EXPECT_EQ(detail::analyseTitle(u"Glass Harbour"_s, u"Extended Mix"_s).version.versionClass,
              selecta::match::VersionClass::Extended);
    EXPECT_EQ(detail::primaryArtist(u"Kestrel Moon feat. Clara Vale"_s), u"Kestrel Moon"_s);
    EXPECT_EQ(detail::primaryArtist(u"Simon & Garfunkel"_s), u"Simon"_s);
    EXPECT_EQ(detail::primaryArtist(u"Above, Beyond"_s), u"Above"_s);
    EXPECT_EQ(detail::primaryArtist(u"  Solo  "_s), u"Solo"_s);
    EXPECT_EQ(detail::versionWords(detail::analyseTitle(u"Glass Harbour (Night Tram Remix)"_s).version),
              u"Night Tram Remix"_s);
}

// --- Resolving through the services ---------------------------------------------------------------------------------

namespace {

const QString kTokenUrl = u"^https://accounts\\.test/api/token$"_s;

class ReleaseResolverTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SpotifyAuth::Config config;
        config.clientId = u"0123456789abcdef0123456789abcdef"_s;
        config.accountsUrl = QUrl(u"https://accounts.test"_s);
        config.openUrl = [](const QUrl &) { return false; };
        auth = std::make_unique<SpotifyAuth>(config, &network, pacer, store, clock);
        spotify = std::make_unique<SpotifyClient>(*auth, &network, pacer, &cache, clock, nullptr,
                                                  QUrl(u"https://api.test/v1"_s));
        deezer = std::make_unique<DeezerClient>(&network, pacer, &cache, clock, nullptr, QUrl(u"https://dz.test"_s));
        resolver = std::make_unique<ReleaseResolver>(spotify.get(), deezer.get());
    }

    void signIn()
    {
        network.enqueue("POST", kTokenUrl,
                        FakeResponse::json(R"({"access_token":"T","token_type":"Bearer","expires_in":3600})"));
        ASSERT_TRUE(selecta::meta::testing::await(auth->signInWithClientSecret("SECRET"))->ok());
    }

    std::optional<Result<Resolution>> resolve(const ResolveQuery &query, Priority priority = Priority::Interactive)
    {
        return awaitAdvancing(resolver->resolve(query, priority), clock, 1s, 10000);
    }

    int countHost(const QString &host) const
    {
        int count = 0;
        for (const auto &request : network.requests()) {
            count += request.url.host() == host ? 1 : 0;
        }
        return count;
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    MemoryMetaCache cache{clock};
    InMemoryTokenStore store;
    std::unique_ptr<SpotifyAuth> auth;
    std::unique_ptr<SpotifyClient> spotify;
    std::unique_ptr<DeezerClient> deezer;
    std::unique_ptr<ReleaseResolver> resolver;
};

} // namespace

TEST_F(ReleaseResolverTest, SpotifyFirstAndDeezerNotNeededWhenSpotifyIsSure)
{
    signIn();
    network.enqueue("GET", u"^https://api\\.test/v1/search\\?"_s,
                    FakeResponse::json(fixture(u"spotify/search_tracks.json"_s)));
    const auto result = resolve(line(kKestrel, u"Glass Harbour (Extended Mix)"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(best(result->value()), u"Glass Harbour - Extended Mix"_s);
    EXPECT_EQ(result->value().confidence, Confidence::Confident);
    EXPECT_EQ(result->value().sourcesTried, QList<Source>{Source::Spotify});
    EXPECT_EQ(countHost(u"dz.test"_s), 0);
    const QUrlQuery sent(network.requests().last().url);
    EXPECT_EQ(sent.queryItemValue(u"q"_s, QUrl::FullyDecoded), u"track:\"Glass Harbour\" artist:\"Kestrel Moon\""_s);
}

TEST_F(ReleaseResolverTest, DeezerAloneWithoutASpotifySignIn)
{
    network.enqueue("GET", u"^https://dz\\.test/search\\?"_s,
                    FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    const auto result = resolve(line(u"Kestrel Moon feat. Clara Vale"_s, u"Glass Harbour (Extended Mix)"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(best(result->value()), u"Glass Harbour (Extended Mix)"_s);
    EXPECT_EQ(result->value().candidates.first().release.source, Source::Deezer);
    EXPECT_EQ(result->value().sourcesTried, QList<Source>{Source::Deezer});
    EXPECT_EQ(countHost(u"api.test"_s), 0);
    EXPECT_EQ(QUrlQuery(network.requests().first().url).queryItemValue(u"q"_s, QUrl::FullyDecoded),
              u"Kestrel Moon Glass Harbour"_s); // the primary artist and the core title
}

TEST_F(ReleaseResolverTest, DeezerFillsInWhenSpotifyHasNothingLikely)
{
    signIn();
    network.enqueue("GET", u"^https://api\\.test/v1/search\\?"_s,
                    FakeResponse::json(R"({"tracks":{"items":[],"total":0}})"));
    network.enqueue("GET", u"^https://dz\\.test/search\\?"_s,
                    FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    const auto result = resolve(line(kKestrel, u"Glass Harbour (Radio Edit)"_s));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(best(result->value()), u"Glass Harbour (Radio Edit)"_s);
    EXPECT_EQ(result->value().sourcesTried, (QList<Source>{Source::Spotify, Source::Deezer}));
    EXPECT_EQ(countHost(u"api.test"_s), 1); // an empty first page: no second search with the version words
}

TEST_F(ReleaseResolverTest, AMissingVersionTriggersOneSearchWithItsWordsPerSource)
{
    signIn();
    network.setDefault("GET", u"^https://api\\.test/v1/search\\?"_s,
                       FakeResponse::json(fixture(u"spotify/search_tracks.json"_s)));
    network.setDefault("GET", u"^https://dz\\.test/search\\?"_s,
                       FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    const auto result = resolve(line(kKestrel, u"Glass Harbour (Dub)"_s));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().confidence, Confidence::None); // nobody has the dub
    EXPECT_EQ(countHost(u"api.test"_s), 2);
    EXPECT_EQ(countHost(u"dz.test"_s), 2);
    QStringList spotifyQueries;
    for (const auto &request : network.requests()) {
        if (request.url.host() == u"api.test"_s) {
            spotifyQueries.append(QUrlQuery(request.url).queryItemValue(u"q"_s, QUrl::FullyDecoded));
        }
    }
    EXPECT_TRUE(spotifyQueries.last().endsWith(u" Dub"_s)) << spotifyQueries.last().toStdString();
}

TEST_F(ReleaseResolverTest, IsrcLinesStopAtAnExactMatch)
{
    signIn();
    network.enqueue("GET", u"^https://api\\.test/v1/search\\?"_s,
                    FakeResponse::json(fixture(u"spotify/search_tracks.json"_s)));
    network.enqueue("GET", u"^https://dz\\.test/track/isrc:GBXYZ2600101$"_s,
                    FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    ResolveQuery query = line(kKestrel, u"Glass Harbour"_s);
    query.isrc = u"GBXYZ2600101"_s;
    const auto result = resolve(query);
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().confidence, Confidence::Exact);
    const Release &chosen = result->value().candidates.first().release;
    EXPECT_EQ(chosen.isrc, u"GBXYZ2600101"_s);
    EXPECT_FALSE(chosen.spotifyId.isEmpty());
    EXPECT_EQ(chosen.deezerId, u"3135553"_s); // merged from Deezer's copy
    EXPECT_EQ(QUrlQuery(network.requests().at(1).url).queryItemValue(u"q"_s, QUrl::FullyDecoded),
              u"isrc:GBXYZ2600101"_s);
    EXPECT_EQ(network.requestCount(), 3); // token, Spotify ISRC search, Deezer ISRC lookup: no text search
}

TEST_F(ReleaseResolverTest, AFailingSourceIsReportedWhileTheOtherAnswers)
{
    signIn();
    FakeResponse limited = FakeResponse::json(R"({"error":{"status":429}})", 429);
    limited.headers.append(header("Retry-After", "7200"));
    network.enqueue("GET", u"^https://api\\.test/v1/search\\?"_s, limited);
    network.enqueue("GET", u"^https://dz\\.test/search\\?"_s,
                    FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    const auto result = resolve(line(kKestrel, u"Glass Harbour (Extended Mix)"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_FALSE(result->value().candidates.isEmpty());
    ASSERT_EQ(result->value().sourceErrors.size(), 1);
    EXPECT_EQ(result->value().sourceErrors.first().code, ErrorCode::RateLimited);
    EXPECT_EQ(countHost(u"api.test"_s), 1); // Spotify isn't asked again for this line
}

TEST_F(ReleaseResolverTest, EverySourceFailingFailsTheResolution)
{
    FakeResponse refused;
    refused.networkError = QNetworkReply::ConnectionRefusedError;
    network.setDefault("GET", u"^https://dz\\.test/search\\?"_s, refused);
    const auto result = resolve(line(kKestrel, u"Glass Harbour"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::Network);
}

TEST_F(ReleaseResolverTest, NothingFoundIsAnEmptyResolutionNotAnError)
{
    network.setDefault("GET", u"^https://dz\\.test/search\\?"_s, FakeResponse::json(R"({"data":[],"total":0})"));
    const auto result = resolve(line(kKestrel, u"Unreleased Dubplate"_s));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_TRUE(result->value().candidates.isEmpty());
    EXPECT_EQ(result->value().confidence, Confidence::None);
}

TEST_F(ReleaseResolverTest, BadLinesAndMissingSources)
{
    const auto empty = resolve(ResolveQuery{});
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->error().code, ErrorCode::InvalidArgument);
    ReleaseResolver nothing(nullptr, nullptr);
    const auto none = awaitAdvancing(nothing.resolve(line(kKestrel, u"Glass Harbour"_s)), clock);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->error().code, ErrorCode::NotConfigured);
}

TEST_F(ReleaseResolverTest, CancellingAbortsTheSearchInFlight)
{
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", u"^https://dz\\.test/search\\?"_s, hang);
    QFuture<Result<Resolution>> future = resolver->resolve(line(kKestrel, u"Glass Harbour"_s));
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    future.cancel();
    ASSERT_TRUE(spinUntil([&] { return network.liveReplies() == 0; }));
    EXPECT_TRUE(future.isCanceled());
}

TEST_F(ReleaseResolverTest, ABackgroundBatchDoesNotDelayAnInteractiveLine)
{
    // Deezer only; each search answers on its own route so the order of requests shows the lanes.
    network.setDefault("GET", u"^https://dz\\.test/search\\?"_s, FakeResponse::json(R"({"data":[],"total":0})"));
    QList<QFuture<Result<Resolution>>> batch;
    for (int i = 0; i < 6; ++i) {
        batch.append(resolver->resolve(line(kKestrel, u"Batch Line %1"_s.arg(i)), Priority::Background));
    }
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; })); // the first batch line went out
    const QFuture<Result<Resolution>> interactive = resolver->resolve(line(kKestrel, u"Wanted Now"_s));
    const auto result = awaitAdvancing(interactive, clock, 1s, 8000);
    ASSERT_TRUE(result.has_value() && result->ok());
    // The interactive line was the second request, ahead of five queued batch lines.
    ASSERT_GE(network.requestCount(), 2);
    EXPECT_EQ(QUrlQuery(network.requests().at(1).url).queryItemValue(u"q"_s, QUrl::FullyDecoded),
              u"Kestrel Moon Wanted Now"_s);
    for (auto &future : batch) {
        future.cancel();
    }
}
