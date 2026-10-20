// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/MetaCache.h"
#include "meta/MusicBrainzClient.h"
#include "meta/detail/MusicBrainzJson.h"
#include "meta/detail/ServiceGate.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"
#include "meta/support/Fixtures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
namespace mb = selecta::meta::musicbrainz;
namespace mbjson = selecta::meta::detail::musicbrainz;
using selecta::base::ManualClock;
using selecta::meta::testing::awaitAdvancing;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::fixture;
using selecta::meta::testing::spinUntil;

namespace {

const QString kRecording = u"5bc8fbbc-bde5-4099-8164-d8399f767c45"_s;
const QString kEdit = u"d76d4330-f144-4bea-b0c1-1fdecb91ce37"_s;
const QString kArtist = u"87b0b125-ec1d-4da0-a6eb-8c9ebd69fe29"_s;
const QString kSingle = u"c6a53877-7733-4bdb-9721-0dff076ce2ef"_s;
const QString kSingleGroup = u"0d464138-a623-4255-bfc1-ea36f17fd374"_s;
const QString kAlbum = u"5f2dd97f-1cfb-40f6-a827-688de6a16a3b"_s;
const QString kCompilation = u"3fd42359-92ed-4f45-9a1a-fe878b33e968"_s;

QJsonObject load(const QString &name)
{
    return QJsonDocument::fromJson(fixture(u"musicbrainz/"_s + name)).object();
}

Release glassHarbour()
{
    Release release;
    release.source = Source::Spotify;
    release.title = u"Glass Harbour - Extended Mix"_s;
    release.artists = {u"Kestrel Moon"_s};
    release.album = u"Glass Harbour"_s;
    release.albumType = u"single"_s;
    release.releaseDate = u"2026-03-13"_s;
    release.releaseDatePrecision = DatePrecision::Day;
    release.durationMs = 391000;
    release.isrc = u"GBXYZ2600101"_s;
    release.upc = u"5056000000024"_s;
    return release;
}

QString mbUrl(const QString &pathRegex)
{
    return u"^https://mb\\.test/ws/2"_s + pathRegex;
}

class MusicBrainzClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        client = std::make_unique<MusicBrainzClient>(&network, pacer, &cache, clock, nullptr,
                                                     QUrl(u"https://mb.test/ws/2"_s));
    }

    template <typename T>
    std::optional<T> wait(const QFuture<T> &future)
    {
        return awaitAdvancing(future, clock, 1200ms, 8000);
    }

    void advance(std::chrono::milliseconds by)
    {
        clock.advance(by);
        selecta::meta::detail::ServiceGate::pollAll();
    }

    // The usual answers of a full enrichment.
    void scriptHappyPath()
    {
        network.enqueue("GET", mbUrl(u"/isrc/GBXYZ2600101\\?"_s),
                        FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
        network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
        network.enqueue("GET", mbUrl(u"/release-group/"_s + kSingleGroup),
                        FakeResponse::json(fixture(u"musicbrainz/genres_release_group.json"_s)));
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    MemoryMetaCache cache{clock};
    std::unique_ptr<MusicBrainzClient> client;
};

} // namespace

TEST(MusicBrainzJson, RecordingsAreMappedAndBrokenIdsDropped)
{
    const QList<mb::Recording> recordings = mbjson::recordings(load(u"isrc_lookup.json"_s));
    ASSERT_EQ(recordings.size(), 2);
    EXPECT_EQ(recordings.at(0).id, kRecording);
    EXPECT_EQ(recordings.at(0).title, u"Glass Harbour (extended mix)"_s);
    EXPECT_EQ(recordings.at(0).lengthMs, 391500);
    EXPECT_EQ(recordings.at(0).artists, QStringList{u"Kestrel Moon"_s});
    EXPECT_EQ(recordings.at(0).artistIds, QStringList{kArtist});
    EXPECT_EQ(recordings.at(1).lengthMs, 240000);
}

TEST(MusicBrainzJson, ReleasesCarryTypesLabelsAndBarcodes)
{
    const QList<mb::ReleaseInfo> releases = mbjson::releases(load(u"releases.json"_s));
    ASSERT_EQ(releases.size(), 5);
    EXPECT_EQ(releases.at(0).secondaryTypes, QStringList{u"Compilation"_s});
    EXPECT_TRUE(releases.at(2).barcode.isEmpty()); // null
    EXPECT_EQ(releases.at(2).status, u"Bootleg"_s);
    EXPECT_EQ(releases.at(4).id, kSingle);
    EXPECT_EQ(releases.at(4).releaseGroupId, kSingleGroup);
    EXPECT_EQ(releases.at(4).primaryType, u"Single"_s);
    ASSERT_EQ(releases.at(4).labels.size(), 2);
    EXPECT_EQ(releases.at(4).date, u"2026-03-13"_s);
}

TEST(MusicBrainzJson, GenresSortedByVotesWithoutZeroVotes)
{
    const QList<mb::Genre> genres = mbjson::genres(load(u"genres_release_group.json"_s));
    ASSERT_EQ(genres.size(), 2);
    EXPECT_EQ(genres.at(0).name, u"house"_s);
    EXPECT_EQ(genres.at(0).count, 3);
    EXPECT_EQ(genres.at(1).name, u"electronic"_s);
}

TEST(MusicBrainzJson, FittingRecordingsByArtistAndLength)
{
    const QList<mb::Recording> recordings = mbjson::recordings(load(u"isrc_lookup.json"_s));
    Release release = glassHarbour();
    QList<mb::Recording> fitting = mbjson::fittingRecordings(recordings, release);
    ASSERT_EQ(fitting.size(), 1); // the 240-s edit is 151 s off
    EXPECT_EQ(fitting.first().id, kRecording);

    release.durationMs.reset(); // unknown length: both qualify
    fitting = mbjson::fittingRecordings(recordings, release);
    EXPECT_EQ(fitting.size(), 2);

    release.artists = {u"Somebody Else"_s};
    EXPECT_TRUE(mbjson::fittingRecordings(recordings, release).isEmpty());

    release.artists = {u"KESTREL MOON"_s};
    release.durationMs = 393000; // within ±3 s of 391.5 s
    EXPECT_EQ(mbjson::fittingRecordings(recordings, release).size(), 1);
}

TEST(MusicBrainzJson, ReleaseChoiceRules)
{
    const QList<mb::ReleaseInfo> releases = mbjson::releases(load(u"releases.json"_s));
    QString why;
    Release release = glassHarbour();
    // (a) barcode = UPC.
    std::optional<mb::ReleaseInfo> chosen = mbjson::chooseRelease(releases, release, &why);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, kSingle);
    EXPECT_TRUE(why.contains(u"barcode"_s));
    // (b) title and year; the bootleg with the same title is passed over.
    release.upc.clear();
    chosen = mbjson::chooseRelease(releases, release, &why);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, kSingle);
    EXPECT_TRUE(why.contains(u"title"_s));
    // The album when the track came from the album.
    release.album = u"Tidal Lines"_s;
    release.albumType = u"album"_s;
    release.releaseDate = u"2025-11-21"_s;
    chosen = mbjson::chooseRelease(releases, release, &why);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, kAlbum);
    // (c) earliest official release of the same type, never a compilation or DJ mix.
    release = glassHarbour();
    release.upc.clear();
    release.album.clear();
    chosen = mbjson::chooseRelease(releases, release, &why);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, kSingle);
    EXPECT_TRUE(why.contains(u"earliest official"_s));
    // A compilation is chosen only for a compilation.
    release.album = u"Club Sounds 2026"_s;
    release.albumType = u"compilation"_s;
    release.releaseDate = u"2026-01-10"_s;
    chosen = mbjson::chooseRelease(releases, release, &why);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, kCompilation);
}

TEST(MusicBrainzJson, CompilationsAndMixesNeverGiveTheLabel)
{
    const QList<mb::ReleaseInfo> releases = mbjson::releases(load(u"releases_compilations_only.json"_s));
    QString why;
    EXPECT_FALSE(mbjson::chooseRelease(releases, glassHarbour(), &why).has_value());
    EXPECT_TRUE(why.contains(u"compilations"_s));
    EXPECT_FALSE(mbjson::chooseRelease({}, glassHarbour(), &why).has_value());
}

TEST(MusicBrainzJson, NoLabelAndNoneAreNoAnswer)
{
    const QList<mb::ReleaseInfo> releases = mbjson::releases(load(u"releases.json"_s));
    const mb::LabelInfo single = mbjson::labelOf(releases.at(4));
    EXPECT_EQ(single.label, u"Lantern Grove Records"_s);
    EXPECT_EQ(single.catalogNumber, u"LGR012"_s);
    const mb::LabelInfo bootleg = mbjson::labelOf(releases.at(2));
    EXPECT_TRUE(bootleg.label.isEmpty());
    EXPECT_TRUE(bootleg.catalogNumber.isEmpty());
}

TEST_F(MusicBrainzClientTest, IsrcLookupSendsTheRightRequest)
{
    network.enqueue("GET", mbUrl(u"/isrc/GBXYZ2600101\\?"_s),
                    FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    const auto result = wait(client->recordingsByIsrc(u"gb-xyz-26-00101"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().size(), 2);
    ASSERT_EQ(network.requestCount(), 1);
    const auto &request = network.requests().first();
    EXPECT_EQ(QUrlQuery(request.url).queryItemValue(u"inc"_s), u"artist-credits"_s);
    EXPECT_EQ(QUrlQuery(request.url).queryItemValue(u"fmt"_s), u"json"_s);
    EXPECT_EQ(request.header("User-Agent"), MusicBrainzClient::userAgent());
    EXPECT_TRUE(MusicBrainzClient::userAgent().contains("( https://github.com/usr-ein/selecta )"));
    ASSERT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.acquired().first(), selecta::governor::Operation::MusicBrainzRequest);
}

TEST_F(MusicBrainzClientTest, UnknownIsrcIsNotFoundAndRemembered)
{
    network.setDefault("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(R"({"error":"Not Found","help":"…"})", 404));
    EXPECT_EQ(wait(client->recordingsByIsrc(u"GBXYZ2699999"_s))->error().code, ErrorCode::NotFound);
    EXPECT_EQ(wait(client->recordingsByIsrc(u"GBXYZ2699999"_s))->error().code, ErrorCode::NotFound);
    EXPECT_EQ(network.requestCount(), 1); // the second answer came from the cache
}

TEST_F(MusicBrainzClientTest, MalformedInputIsRefusedLocally)
{
    EXPECT_EQ(wait(client->recordingsByIsrc(u"nope"_s))->error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(wait(client->releasesOfRecording(u"../etc"_s))->error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(wait(client->genres(mb::Entity::Artist, u"x"_s))->error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(wait(client->searchRecordings(u""_s, u"Kestrel Moon"_s, 1000))->error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(MusicBrainzClientTest, ReleasesAndGenresRequests)
{
    network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
    const auto releases = wait(client->releasesOfRecording(kRecording));
    ASSERT_TRUE(releases.has_value() && releases->ok());
    EXPECT_EQ(releases->value().size(), 5);
    const QUrlQuery query(network.requests().first().url);
    EXPECT_EQ(query.queryItemValue(u"recording"_s), kRecording);
    EXPECT_EQ(query.queryItemValue(u"inc"_s, QUrl::FullyDecoded).replace(u' ', u'+'), u"labels+release-groups"_s);
    EXPECT_EQ(query.queryItemValue(u"limit"_s), u"100"_s);

    network.enqueue("GET", mbUrl(u"/release-group/"_s + kSingleGroup),
                    FakeResponse::json(fixture(u"musicbrainz/genres_release_group.json"_s)));
    const auto genres = wait(client->genres(mb::Entity::ReleaseGroup, kSingleGroup));
    ASSERT_TRUE(genres.has_value() && genres->ok());
    ASSERT_EQ(genres->value().size(), 2);
    EXPECT_EQ(genres->value().first().name, u"house"_s);
}

TEST_F(MusicBrainzClientTest, RecordingQueryIsEscapedLucene)
{
    EXPECT_EQ(MusicBrainzClient::recordingQuery(u"Glass Harbour"_s, u"Kestrel Moon"_s, 391000),
              u"recording:\"Glass Harbour\" AND artist:\"Kestrel Moon\" AND dur:[388000 TO 394000]"_s);
    EXPECT_EQ(MusicBrainzClient::recordingQuery(u"Say \"Hi\" \\o/"_s, u"A & B"_s, std::nullopt),
              u"recording:\"Say \\\"Hi\\\" \\\\o/\" AND artist:\"A & B\""_s);
    EXPECT_FALSE(MusicBrainzClient::recordingQuery(u"x"_s, u"y"_s, 2000).contains(u"dur:"_s)); // too short to window
}

TEST_F(MusicBrainzClientTest, EnrichFindsLabelCatalogueNumberAndGenres)
{
    scriptHappyPath();
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    const mb::Enrichment &e = result->value();
    EXPECT_EQ(e.recordingId, kRecording);
    EXPECT_EQ(e.releaseId, kSingle);
    EXPECT_EQ(e.releaseGroupId, kSingleGroup);
    EXPECT_EQ(e.label, u"Lantern Grove Records"_s);
    EXPECT_EQ(e.catalogNumber, u"LGR012"_s);
    EXPECT_EQ(e.genres, (QStringList{u"house"_s, u"electronic"_s}));
    EXPECT_FALSE(e.reasons.isEmpty());
    EXPECT_EQ(network.requestCount(), 3);

    const Release applied = MusicBrainzClient::applied(glassHarbour(), e);
    EXPECT_EQ(applied.label, u"Lantern Grove Records"_s);
    EXPECT_EQ(applied.labelSource, Source::MusicBrainz);
    EXPECT_EQ(applied.genres.first(), u"house"_s);
    EXPECT_EQ(applied.genreSource, Source::MusicBrainz);
    EXPECT_EQ(applied.musicBrainzReleaseId, kSingle);
    EXPECT_EQ(applied.title, glassHarbour().title); // nothing else touched
}

TEST_F(MusicBrainzClientTest, RequestsStartAtLeastElevenHundredMillisecondsApart)
{
    scriptHappyPath();
    const QFuture<Result<mb::Enrichment>> future = client->enrich(glassHarbour());
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    QTest::qWait(30);
    EXPECT_EQ(network.requestCount(), 1);
    advance(1099ms);
    QTest::qWait(30);
    EXPECT_EQ(network.requestCount(), 1);
    advance(1ms);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 2; }));
    advance(1100ms);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 3; }));
    ASSERT_TRUE(spinUntil([&] { return future.isFinished(); }));
}

TEST(MusicBrainzJson, GenreTiesAreBrokenByTheNextLevelsVotes)
{
    // Seen live (2026-10-20): "Glue" had one vote each for disco, electronic, house and techno, which MusicBrainz lists
    // alphabetically: "disco" was tagged.
    const QList<mb::Genre> group = {{u"disco"_s, 1}, {u"electronic"_s, 1}, {u"house"_s, 1}, {u"techno"_s, 1}};
    EXPECT_TRUE(mbjson::leadingGenresTied({group}));
    EXPECT_EQ(mbjson::rankedGenres({group}).constFirst(), u"disco"_s); // nothing to tell them apart yet
    const QList<mb::Genre> recording = {{u"house"_s, 1}, {u"techno"_s, 1}};
    EXPECT_TRUE(mbjson::leadingGenresTied({group, recording}));
    const QList<mb::Genre> artist = {{u"electronic"_s, 9}, {u"house"_s, 4}, {u"techno"_s, 6}, {u"ambient"_s, 12}};
    // Lexicographic: the recording's votes first, then the artist's; genres the first level lacks never enter.
    EXPECT_EQ(mbjson::rankedGenres({group, recording, artist}),
              (QStringList{u"techno"_s, u"house"_s, u"electronic"_s, u"disco"_s}));
    EXPECT_FALSE(mbjson::leadingGenresTied({group, recording, artist}));
    EXPECT_EQ(mbjson::rankedGenres({group, artist}).constFirst(), u"electronic"_s);
    // The first level's own votes always come first.
    const QList<mb::Genre> voted = {{u"house"_s, 3}, {u"disco"_s, 1}};
    EXPECT_FALSE(mbjson::leadingGenresTied({voted}));
    EXPECT_EQ(mbjson::rankedGenres({voted, {{u"disco"_s, 50}}}), (QStringList{u"house"_s, u"disco"_s}));
    EXPECT_TRUE(mbjson::rankedGenres({}).isEmpty());
    EXPECT_FALSE(mbjson::leadingGenresTied({}));
}

TEST_F(MusicBrainzClientTest, ATieAtTheTopAsksTheNextLevelsUntilOneBreaksIt)
{
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
    network.enqueue("GET", mbUrl(u"/release-group/"_s + kSingleGroup),
                    FakeResponse::json(R"({"genres":[{"name":"disco","count":1},{"name":"electronic","count":1},)"
                                       R"({"name":"house","count":1},{"name":"techno","count":1}]})"));
    network.enqueue("GET", mbUrl(u"/recording/"_s + kRecording), FakeResponse::json(R"({"genres":[]})"));
    network.enqueue("GET", mbUrl(u"/artist/"_s),
                    FakeResponse::json(R"({"genres":[{"name":"electronic","count":7},{"name":"house","count":3}]})"));
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().genres, (QStringList{u"electronic"_s, u"house"_s, u"disco"_s, u"techno"_s}));
    EXPECT_TRUE(result->value().reasons.join(u" "_s).contains(u"tie broken by the artist"_s));
    EXPECT_EQ(network.requestCount(), 5);
}

TEST_F(MusicBrainzClientTest, NoTieNoExtraRequestAndAFailedTieBreakKeepsTheGenres)
{
    scriptHappyPath(); // house 3, electronic 1
    ASSERT_TRUE(wait(client->enrich(glassHarbour()))->ok());
    EXPECT_EQ(network.requestCount(), 3);

    cache.removePrefix({});
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
    network.enqueue("GET", mbUrl(u"/release-group/"_s + kSingleGroup),
                    FakeResponse::json(R"({"genres":[{"name":"disco","count":2},{"name":"house","count":2}]})"));
    network.setDefault("GET", mbUrl(u"/recording/"_s), FakeResponse::json("{}", 500));
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().genres, (QStringList{u"disco"_s, u"house"_s}));
    EXPECT_TRUE(result->value().reasons.join(u" "_s).contains(u"genres unavailable"_s));
}

TEST_F(MusicBrainzClientTest, GenresFallBackToTheRecordingThenTheArtist)
{
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
    network.enqueue("GET", mbUrl(u"/release-group/"_s), FakeResponse::json(R"({"genres":[]})"));
    network.enqueue("GET", mbUrl(u"/recording/"_s + kRecording), FakeResponse::json(R"({"genres":[]})"));
    network.enqueue("GET", mbUrl(u"/artist/"_s + kArtist),
                    FakeResponse::json(R"({"genres":[{"name":"deep house","count":4}]})"));
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().genres, QStringList{u"deep house"_s});
    EXPECT_EQ(network.requestCount(), 5);
}

TEST_F(MusicBrainzClientTest, NoGenreVotesAnywhereIsFine)
{
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
    network.setDefault("GET", mbUrl(u"/(release-group|recording|artist)/"_s), FakeResponse::json(R"({"genres":[]})"));
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_TRUE(result->value().genres.isEmpty());
    EXPECT_EQ(result->value().label, u"Lantern Grove Records"_s);
    EXPECT_TRUE(result->value().reasons.last().contains(u"no genre"_s));
}

TEST_F(MusicBrainzClientTest, UnknownIsrcFallsBackToAStrongSearchHit)
{
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(R"({"error":"Not Found"})", 404));
    network.enqueue("GET", mbUrl(u"/recording\\?"_s),
                    FakeResponse::json(fixture(u"musicbrainz/search_recordings.json"_s)));
    network.enqueue("GET", mbUrl(u"/release\\?"_s), FakeResponse::json(fixture(u"musicbrainz/releases.json"_s)));
    network.enqueue("GET", mbUrl(u"/release-group/"_s),
                    FakeResponse::json(fixture(u"musicbrainz/genres_release_group.json"_s)));
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().recordingId, kRecording);
    const QString query = QUrlQuery(network.requests().at(1).url).queryItemValue(u"query"_s, QUrl::FullyDecoded);
    EXPECT_TRUE(query.contains(u"recording:\"Glass Harbour\""_s)) << query.toStdString(); // version stripped
    EXPECT_TRUE(query.contains(u"dur:[388000 TO 394000]"_s)) << query.toStdString();
}

TEST_F(MusicBrainzClientTest, WeakSearchHitsAreNotEnough)
{
    Release release = glassHarbour();
    release.isrc.clear();
    QJsonObject search = load(u"search_recordings.json"_s);
    QJsonArray recordings = search.value("recordings"_L1).toArray();
    recordings.removeFirst(); // only the score-72 hit remains
    search.insert("recordings"_L1, recordings);
    network.enqueue("GET", mbUrl(u"/recording\\?"_s), FakeResponse::json(QJsonDocument(search).toJson()));
    const auto result = wait(client->enrich(release));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotFound);
}

TEST_F(MusicBrainzClientTest, NothingToSearchWithIsNotFound)
{
    Release release;
    release.title = u"Untitled"_s;
    const auto result = wait(client->enrich(release));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotFound);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(MusicBrainzClientTest, RecordingsOfTheWrongLengthAreNotTheRelease)
{
    Release release = glassHarbour();
    release.durationMs = 500000;
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    const auto result = wait(client->enrich(release));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotFound);
    EXPECT_EQ(network.requestCount(), 1);
}

TEST_F(MusicBrainzClientTest, CompilationsOnlyGiveNoLabelButGenresStillCome)
{
    network.enqueue("GET", mbUrl(u"/isrc/"_s), FakeResponse::json(fixture(u"musicbrainz/isrc_lookup.json"_s)));
    network.enqueue("GET", mbUrl(u"/release\\?"_s),
                    FakeResponse::json(fixture(u"musicbrainz/releases_compilations_only.json"_s)));
    network.enqueue("GET", mbUrl(u"/recording/"_s + kRecording),
                    FakeResponse::json(R"({"genres":[{"name":"house","count":1}]})"));
    const auto result = wait(client->enrich(glassHarbour()));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_TRUE(result->value().label.isEmpty());
    EXPECT_TRUE(result->value().releaseId.isEmpty());
    EXPECT_EQ(result->value().genres, QStringList{u"house"_s});
    const Release applied = MusicBrainzClient::applied(glassHarbour(), result->value());
    EXPECT_FALSE(applied.labelSource.has_value());
}

TEST_F(MusicBrainzClientTest, ServiceUnavailableIsRetriedThenGivenUp)
{
    network.setDefault("GET", mbUrl(u"/isrc/"_s), FakeResponse::json("{}", 503));
    const auto result = wait(client->recordingsByIsrc(u"GBXYZ2600101"_s));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::RateLimited);
    EXPECT_EQ(network.requestCount(), 3); // at most 3 tries in all (§5.3)
}

TEST_F(MusicBrainzClientTest, CancelledEnrichmentStops)
{
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", mbUrl(u"/isrc/"_s), hang);
    QFuture<Result<mb::Enrichment>> future = client->enrich(glassHarbour());
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    future.cancel();
    ASSERT_TRUE(spinUntil([&] { return network.liveReplies() == 0; }));
    advance(10s);
    QTest::qWait(30);
    EXPECT_EQ(network.requestCount(), 1);
}
