// SPDX-License-Identifier: GPL-3.0-or-later
// Album ownership (note §8): folder ↔ album by names and duration signature, the Hungarian track mapping, coverage.
#include "match/AlbumMatcher.h"

#include "MatchTestSupport.h"
#include "match/MatchIndex.h"
#include "match/RemotePathParser.h"
#include "match/detail/Hungarian.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <numeric>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::match::AlbumMatcher;
using selecta::match::AlbumMatchResult;
using selecta::match::LibraryAlbum;
using selecta::match::LibraryTrack;
using selecta::match::MatchIndex;
using selecta::match::MatchIndexInput;
using selecta::match::MatchOptions;
using selecta::match::RemoteAlbumFile;
using selecta::match::RemotePathParser;
using selecta::match::Source;
using selecta::match::Tier;

namespace {

// --- The assignment ------------------------------------------------------------------------------------------------

double bruteForce(const std::vector<double> &cost, qsizetype rows, qsizetype cols)
{
    // Every injective map of the smaller side into the larger.
    const bool transposed = rows > cols;
    const qsizetype small = transposed ? cols : rows;
    const qsizetype large = transposed ? rows : cols;
    std::vector<qsizetype> columns(static_cast<std::size_t>(large));
    std::iota(columns.begin(), columns.end(), 0);
    double best = std::numeric_limits<double>::infinity();
    do {
        double total = 0.0;
        for (qsizetype i = 0; i < small; ++i) {
            const qsizetype j = columns[static_cast<std::size_t>(i)];
            total += transposed ? cost[static_cast<std::size_t>((j * cols) + i)]
                                : cost[static_cast<std::size_t>((i * cols) + j)];
        }
        best = std::min(best, total);
    } while (std::ranges::next_permutation(columns).found);
    return best;
}

TEST(Hungarian, FindsTheMinimumCostAssignment) // against every permutation
{
    selecta::match::test::Random random(808);
    for (int round = 0; round < 300; ++round) {
        const qsizetype rows = 1 + random.below(5);
        const qsizetype cols = 1 + random.below(5);
        std::vector<double> cost(static_cast<std::size_t>(rows * cols));
        for (double &c : cost) {
            c = static_cast<double>(random.below(1000)) / 100.0;
        }
        const auto pairs = selecta::match::detail::minimumCostAssignment(cost, rows, cols);
        ASSERT_EQ(static_cast<qsizetype>(pairs.size()), std::min(rows, cols));
        double total = 0.0;
        std::vector<bool> rowUsed(static_cast<std::size_t>(rows), false);
        std::vector<bool> colUsed(static_cast<std::size_t>(cols), false);
        for (const auto &[i, j] : pairs) {
            ASSERT_FALSE(rowUsed[static_cast<std::size_t>(i)]);
            ASSERT_FALSE(colUsed[static_cast<std::size_t>(j)]);
            rowUsed[static_cast<std::size_t>(i)] = true;
            colUsed[static_cast<std::size_t>(j)] = true;
            total += cost[static_cast<std::size_t>((i * cols) + j)];
        }
        ASSERT_NEAR(total, bruteForce(cost, rows, cols), 1e-9) << rows << "×" << cols;
    }
}

TEST(Hungarian, EmptyInputGivesNothing)
{
    EXPECT_TRUE(selecta::match::detail::minimumCostAssignment({}, 0, 3).empty());
    EXPECT_TRUE(selecta::match::detail::minimumCostAssignment({1.0}, 2, 2).empty()); // too few costs
}

// --- Albums --------------------------------------------------------------------------------------------------------

struct AlbumSpec
{
    quint64 id = 0;
    QString artist;
    QString title;
    int year = 0;
    QStringList titles;
    QList<int> seconds;
    Source source = Source::Collection;
};

struct Library
{
    QList<AlbumSpec> albums;
    QList<LibraryTrack> singles;
};

std::shared_ptr<const MatchIndex> indexOf(const Library &library)
{
    MatchIndexInput input;
    quint64 trackId = 1000;
    for (const AlbumSpec &spec : library.albums) {
        LibraryAlbum album;
        album.id = spec.id;
        album.source = spec.source;
        album.artist = spec.artist;
        album.title = spec.title;
        album.year = spec.year;
        input.albums.append(album);
        for (qsizetype i = 0; i < spec.titles.size(); ++i) {
            LibraryTrack track;
            track.id = trackId++;
            track.source = spec.source;
            track.artist = spec.artist;
            track.title = spec.titles.at(i);
            track.album = spec.title;
            track.albumArtist = spec.artist;
            track.albumId = spec.id;
            track.trackNumber = static_cast<int>(i) + 1;
            track.durationMs = (static_cast<qint64>(spec.seconds.at(i)) * 1000) + 400;
            track.quality = {selecta::match::Codec::Flac, 0, true, 44100, 16};
            input.tracks.append(track);
        }
    }
    input.tracks += library.singles;
    input.generation = 3;
    return MatchIndex::build(input);
}

// A remote folder: file names and lengths, parsed together like a search response.
QList<RemoteAlbumFile> folder(const QString &path, const QStringList &names, const QList<int> &seconds,
                              const std::shared_ptr<const MatchIndex> &index)
{
    const QList<selecta::match::ParsedRemote> parsed =
        RemotePathParser::parseFolder(path, names, {&index->artists(), nullptr});
    QList<RemoteAlbumFile> files;
    for (qsizetype i = 0; i < names.size(); ++i) {
        RemoteAlbumFile file;
        file.parsed = parsed.at(i);
        file.file.peer = u"peer"_s;
        file.file.path = path + u'\\' + names.at(i);
        file.file.size = 30'000'000;
        file.file.attributes.durationSec = i < seconds.size() ? seconds.at(i) : 0;
        files.append(file);
    }
    return files;
}

QStringList islesTitles()
{
    return {u"Atlas"_s, u"Cazenove"_s, u"Apricots"_s, u"Saku"_s, u"Lido"_s, u"Rever"_s};
}

QList<int> islesSeconds()
{
    return {290, 245, 312, 268, 199, 334};
}

Library islesLibrary()
{
    Library library;
    library.albums.append({1, u"Bicep"_s, u"Isles"_s, 2021, islesTitles(), islesSeconds()});
    library.albums.append({2,
                           u"Bonobo"_s,
                           u"Fragments"_s,
                           2022,
                           {u"Polyghost"_s, u"Rosewood"_s, u"Shadows"_s, u"Otomo"_s, u"Tides"_s},
                           {244, 302, 276, 381, 231}});
    return library;
}

QStringList numbered(const QString &artist, const QStringList &titles, const QString &extension)
{
    QStringList names;
    for (qsizetype i = 0; i < titles.size(); ++i) {
        names.append(u"%1 - %2 - %3.%4"_s.arg(i + 1, 2, 10, QChar(u'0')).arg(artist, titles.at(i), extension));
    }
    return names;
}

TEST(AlbumMatcher, TheSameAlbumIsInCollectionExactly)
{
    const auto index = indexOf(islesLibrary());
    const auto files =
        folder(u"Music\\Bicep - Isles (2021)"_s, numbered(u"Bicep"_s, islesTitles(), u"flac"_s), islesSeconds(), index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files, MatchOptions{true});
    ASSERT_TRUE(result.best.found) << result.explanation.join(u'\n').toStdString();
    EXPECT_EQ(result.best.albumId, 1U);
    EXPECT_TRUE(result.best.inCollection);
    EXPECT_EQ(result.best.tier, Tier::Exact);
    EXPECT_FALSE(result.best.signatureOnly);
    EXPECT_LE(result.best.distance, 0.05);
    EXPECT_EQ(result.best.mapping.size(), 6);
    EXPECT_EQ(result.best.mappedLikely, 6);
    EXPECT_EQ(result.remoteTracks, 6);
    EXPECT_EQ(result.owned, 6);
    EXPECT_EQ(result.ownedAsSingles, 0);
    for (const auto &mapping : result.best.mapping) {
        EXPECT_EQ(mapping.trackId, 1000U + static_cast<quint64>(mapping.remoteIndex)); // file i ↔ track i
    }
    EXPECT_FALSE(result.explanation.isEmpty());
}

TEST(AlbumMatcher, ADurationSignatureFindsAnAlbumWithUnparseableNames) // T48
{
    Library library;
    const QStringList titles{u"Один"_s,  u"Два"_s,  u"Три"_s,    u"Четыре"_s, u"Пять"_s,
                             u"Шесть"_s, u"Семь"_s, u"Восемь"_s, u"Девять"_s, u"Десять"_s};
    const QList<int> seconds{201, 233, 187, 305, 262, 244, 199, 318, 276, 223};
    library.albums.append({7, u"Кино"_s, u"Группа крови"_s, 1988, titles, seconds});
    library.albums.append(islesLibrary().albums.front());
    const auto index = indexOf(library);
    QStringList names;
    QList<int> remoteSeconds;
    for (qsizetype i = 0; i < 10; ++i) {
        names.append(u"%1.flac"_s.arg(i + 1, 2, 10, QChar(u'0')));
        remoteSeconds.append(seconds.at(i) + ((i % 2 == 0) ? 2 : -1)); // within ±2 s of the owned lengths
    }
    const auto files = folder(u"Share\\Неизвестный альбом"_s, names, remoteSeconds, index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files, MatchOptions{true});
    ASSERT_TRUE(result.best.found) << result.explanation.join(u'\n').toStdString();
    EXPECT_EQ(result.best.albumId, 7U);
    EXPECT_TRUE(result.best.inCollection);
    EXPECT_EQ(result.best.tier, Tier::Likely);
    EXPECT_TRUE(result.best.signatureOnly);
    EXPECT_EQ(result.best.mappedLikely, 10);
    EXPECT_EQ(result.owned, 0); // no file is owned by its name
    for (const auto &mapping : result.best.mapping) {
        EXPECT_EQ(mapping.trackId, 1000U + static_cast<quint64>(mapping.remoteIndex)) << "track numbers and lengths";
    }
}

TEST(AlbumMatcher, NamesThatContradictASignatureWin)
{
    const auto index = indexOf(islesLibrary());
    // Isles' lengths under another artist's album of other titles: the signature alone mustn't claim it.
    const QStringList titles{u"Mercury"_s, u"Venus"_s, u"Earth"_s, u"Mars"_s, u"Jupiter"_s, u"Saturn"_s};
    const auto files = folder(u"Music\\Holst Ensemble - The Planets"_s, numbered(u"Holst Ensemble"_s, titles, u"mp3"_s),
                              islesSeconds(), index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files, MatchOptions{true});
    EXPECT_FALSE(result.best.inCollection) << result.explanation.join(u'\n').toStdString();
    EXPECT_FALSE(result.best.signatureOnly);
    EXPECT_EQ(result.owned, 0);
}

TEST(AlbumMatcher, CoverageCountsTracksOwnedAsSingles)
{
    Library library = islesLibrary();
    // "Hawk" and "Glue" are owned as singles; the remote folder holds Isles plus them as bonus tracks.
    for (const auto &[id, title, seconds] : {std::tuple{50U, u"Hawk"_s, 250}, std::tuple{51U, u"Glue"_s, 269}}) {
        LibraryTrack single;
        single.id = id;
        single.artist = u"Bicep"_s;
        single.title = title;
        single.durationMs = (static_cast<qint64>(seconds) * 1000) + 300;
        single.source = Source::Inbox;
        library.singles.append(single);
    }
    const auto index = indexOf(library);
    QStringList titles = islesTitles();
    titles += {u"Hawk"_s, u"Glue"_s, u"Unreleased"_s};
    QList<int> seconds = islesSeconds();
    seconds += {250, 269, 300};
    const auto files =
        folder(u"Music\\Bicep - Isles (Deluxe)"_s, numbered(u"Bicep"_s, titles, u"flac"_s), seconds, index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files, MatchOptions{true});
    ASSERT_TRUE(result.best.found) << result.explanation.join(u'\n').toStdString();
    EXPECT_EQ(result.best.albumId, 1U);
    EXPECT_EQ(result.remoteTracks, 9);
    EXPECT_EQ(result.owned, 8);          // "owned 8/9
    EXPECT_EQ(result.ownedAsSingles, 2); //  (2 as singles)"
    EXPECT_EQ(result.best.mappedLikely, 6);
    EXPECT_FALSE(result.best.inCollection); // 6 of 9 remote tracks on the album: below 90 %
    EXPECT_EQ(result.trackTiers.size(), 9);
    EXPECT_EQ(result.trackTiers.at(8), Tier::None); // "Unreleased" isn't owned anywhere
}

TEST(AlbumMatcher, AnAlbumMissingTracksIsNotInCollection)
{
    const auto index = indexOf(islesLibrary());
    const QStringList titles = islesTitles().mid(0, 3); // half of the album
    const auto files =
        folder(u"Music\\Bicep - Isles"_s, numbered(u"Bicep"_s, titles, u"mp3"_s), islesSeconds().mid(0, 3), index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files);
    ASSERT_TRUE(result.best.found);
    EXPECT_EQ(result.best.albumId, 1U);
    EXPECT_EQ(result.best.mappedLikely, 3);
    // Every remote track is on the album, but three local tracks are missing: distance above 0.20.
    EXPECT_GT(result.best.distance, selecta::match::kAlbumInCollectionDistance);
    EXPECT_FALSE(result.best.inCollection);
    EXPECT_EQ(result.owned, 3);
    EXPECT_EQ(result.ownedAsSingles, 0);
}

TEST(AlbumMatcher, AFolderOfUnknownMusicMatchesNothing)
{
    const auto index = indexOf(islesLibrary());
    const QStringList titles{u"Nowhere"_s, u"Somewhere"_s, u"Elsewhere"_s};
    const auto files = folder(u"Incoming\\Unknown Band - Demos"_s, numbered(u"Unknown Band"_s, titles, u"mp3"_s),
                              {200, 210, 220}, index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files);
    EXPECT_FALSE(result.best.found);
    EXPECT_FALSE(result.best.inCollection);
    EXPECT_EQ(result.owned, 0);
    EXPECT_EQ(result.remoteTracks, 3);
}

TEST(AlbumMatcher, CompanionFilesAreNotTracks)
{
    const auto index = indexOf(islesLibrary());
    QStringList names = numbered(u"Bicep"_s, islesTitles(), u"flac"_s);
    names.prepend(u"cover.jpg"_s);
    names.append(u"Bicep - Isles.cue"_s);
    QList<int> seconds = islesSeconds();
    seconds.prepend(0);
    const auto files = folder(u"Music\\Bicep - Isles (2021)"_s, names, seconds, index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files);
    EXPECT_EQ(result.remoteTracks, 6);
    ASSERT_EQ(result.trackTiers.size(), 8);
    EXPECT_EQ(result.trackTiers.front(), Tier::None);
    EXPECT_EQ(result.trackTiers.back(), Tier::None);
    EXPECT_EQ(result.trackTiers.at(1), Tier::Exact);
    EXPECT_TRUE(result.best.inCollection);
    for (const auto &mapping : result.best.mapping) {
        EXPECT_GE(mapping.remoteIndex, 1); // indexes into the files passed in
        EXPECT_LE(mapping.remoteIndex, 6);
    }
}

TEST(AlbumMatcher, PrefersTheCollectionCopyOfAnAlbum)
{
    Library library = islesLibrary();
    AlbumSpec inbox = library.albums.front();
    inbox.id = 9;
    inbox.source = Source::Inbox;
    library.albums.append(inbox);
    const auto index = indexOf(library);
    const auto files =
        folder(u"Music\\Bicep - Isles (2021)"_s, numbered(u"Bicep"_s, islesTitles(), u"flac"_s), islesSeconds(), index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files);
    ASSERT_TRUE(result.best.found);
    EXPECT_EQ(result.best.albumId, 1U);
    EXPECT_EQ(result.best.source, Source::Collection);
}

TEST(AlbumMatcher, BoundsHugeFolders)
{
    const auto index = indexOf(islesLibrary());
    QStringList names;
    for (int i = 0; i < 150; ++i) {
        names.append(u"%1 - Track %2.mp3"_s.arg(i + 1, 3, 10, QChar(u'0')).arg(i + 1));
    }
    const auto files = folder(u"Dump"_s, names, {}, index);
    const AlbumMatchResult result = AlbumMatcher(index).match(files);
    EXPECT_EQ(result.remoteTracks, static_cast<int>(selecta::match::kMaxAlbumTracks));
    EXPECT_EQ(result.trackTiers.size(), 150);
}

TEST(AlbumMatcher, WithoutASnapshotNothingMatches)
{
    const AlbumMatcher matcher(nullptr);
    const AlbumMatchResult result = matcher.match({}, MatchOptions{true});
    EXPECT_FALSE(result.best.found);
    EXPECT_FALSE(result.explanation.isEmpty());
}

} // namespace
