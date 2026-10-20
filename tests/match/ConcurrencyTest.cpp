// SPDX-License-Identifier: GPL-3.0-or-later
// Thread safety (contract "Threading"): one immutable MatchIndex shared by many threads, snapshots swapped while
// readers keep using the old one. Run under the tsan preset, this proves the claims in the headers.
#include "MatchTestSupport.h"
#include "match/AlbumMatcher.h"
#include "match/ArtistDictionary.h"
#include "match/MatchIndex.h"
#include "match/Matcher.h"
#include "match/QueryPlanner.h"
#include "match/ReleaseScorer.h"
#include "match/RemotePathParser.h"

#include <atomic>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::match::LibraryTrack;
using selecta::match::Matcher;
using selecta::match::MatchIndex;
using selecta::match::MatchIndexInput;
using selecta::match::MatchResult;
using selecta::match::ParseContext;
using selecta::match::ParsedRemote;
using selecta::match::QueryPlanner;
using selecta::match::ReleaseReference;
using selecta::match::ReleaseScorer;
using selecta::match::RemoteFile;
using selecta::match::RemotePathParser;
using selecta::match::Tier;

namespace {

constexpr int kThreads = 8;
constexpr int kRounds = 1;

MatchIndexInput library(int tracks, quint64 generation)
{
    MatchIndexInput input;
    input.generation = generation;
    for (int i = 0; i < tracks; ++i) {
        LibraryTrack track;
        track.id = static_cast<quint64>(i) + 1;
        track.artist = u"Artist %1"_s.arg(i % 50);
        track.title = u"Song Number %1"_s.arg(i);
        track.durationMs = 180000 + (i * 1000);
        input.tracks.append(track);
    }
    return input;
}

struct Probe
{
    QString path;
    RemoteFile file;
};

QList<Probe> probes(int count)
{
    QList<Probe> list;
    for (int i = 0; i < count; ++i) {
        Probe probe;
        probe.path = (i % 3 == 0) ? u"Music\\Artist %1\\Album\\%2 - Song Numbre %3.mp3"_s.arg(i % 50).arg(i % 20).arg(i)
                                  : u"Artist %1 - Song Number %2.mp3"_s.arg(i % 50).arg(i);
        probe.file.path = probe.path;
        probe.file.attributes.durationSec = 180 + i;
        list.append(probe);
    }
    return list;
}

TEST(Concurrency, SharedSnapshotGivesTheSameAnswersOnEveryThread)
{
    const std::shared_ptr<const MatchIndex> index = MatchIndex::build(library(2000, 1));
    const QList<Probe> list = probes(80);
    // Expected answers, computed on this thread first.
    QList<MatchResult> expected;
    for (const Probe &probe : list) {
        const ParsedRemote parsed = RemotePathParser::parse(probe.path, {}, ParseContext{&index->artists(), nullptr});
        expected.append(Matcher(index).match(parsed, probe.file));
    }
    ASSERT_GT(std::ranges::count_if(expected, [](const MatchResult &r) { return r.collection.tier != Tier::None; }),
              40);

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    const ReleaseScorer scorer(ReleaseReference{{u"Artist 7"_s}, u"Song Number 7"_s, {}, 187000});
    const QueryPlanner planner({u"forbidden"_s});
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            const Matcher matcher(index); // each thread its own Matcher, all on one snapshot
            for (int round = 0; round < kRounds; ++round) {
                for (qsizetype i = t % 2; i < list.size(); i += 2) {
                    const Probe &probe = list.at(i);
                    const ParsedRemote parsed =
                        RemotePathParser::parse(probe.path, {}, ParseContext{&index->artists(), nullptr});
                    const MatchResult result = matcher.match(parsed, probe.file);
                    const MatchResult &want = expected.at(i);
                    if (result.collection.tier != want.collection.tier || result.collection.id != want.collection.id) {
                        mismatches.fetch_add(1);
                    }
                    (void)scorer.score(parsed, probe.file.attributes.durationSec);
                    (void)planner.plan(scorer.reference());
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(Concurrency, ReadersKeepTheirSnapshotWhileANewOneIsPublished)
{
    std::mutex publish;
    std::shared_ptr<const MatchIndex> current = MatchIndex::build(library(500, 1));
    const auto snapshot = [&publish, &current] {
        const std::scoped_lock lock(publish);
        return current;
    };
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    readers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&] {
            while (!done.load()) {
                const std::shared_ptr<const MatchIndex> index = snapshot();
                const ParsedRemote parsed = RemotePathParser::parse(u"Artist 3 - Song Number 3.mp3"_s, {},
                                                                    ParseContext{&index->artists(), nullptr});
                RemoteFile file;
                file.path = parsed.path;
                file.attributes.durationSec = 183;
                const MatchResult result = Matcher(index).match(parsed, file);
                if (result.collection.tier != Tier::Exact || result.generation != index->generation()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (quint64 generation = 2; generation <= 6; ++generation) {
        std::shared_ptr<const MatchIndex> next = MatchIndex::build(library(500, generation));
        const std::scoped_lock lock(publish);
        current = std::move(next);
    }
    done.store(true);
    for (std::thread &reader : readers) {
        reader.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(snapshot()->generation(), 6U);
}

TEST(Concurrency, OneAlbumMatcherServesManyThreads)
{
    MatchIndexInput input;
    for (int a = 0; a < 20; ++a) {
        selecta::match::LibraryAlbum album;
        album.id = static_cast<quint64>(a) + 1;
        album.artist = u"Band %1"_s.arg(a);
        album.title = u"Record %1"_s.arg(a);
        input.albums.append(album);
        for (int t = 0; t < 8; ++t) {
            LibraryTrack track;
            track.id = (static_cast<quint64>(a) * 100) + static_cast<quint64>(t);
            track.artist = album.artist;
            track.title = u"Piece %1 of %2"_s.arg(t).arg(a);
            track.album = album.title;
            track.albumId = album.id;
            track.trackNumber = t + 1;
            track.durationMs = 150000 + (a * 7000) + (t * 11000);
            input.tracks.append(track);
        }
    }
    const std::shared_ptr<const MatchIndex> index = MatchIndex::build(input);
    const auto folderOf = [&index](int a) {
        QStringList names;
        for (int t = 0; t < 8; ++t) {
            names.append(u"%1 - Band %2 - Piece %3 of %2.mp3"_s.arg(t + 1, 2, 10, QChar(u'0')).arg(a).arg(t));
        }
        const QString path = u"Music\\Band %1 - Record %1"_s.arg(a);
        const QList<ParsedRemote> parsed = RemotePathParser::parseFolder(path, names, {&index->artists(), nullptr});
        QList<selecta::match::RemoteAlbumFile> files;
        for (int t = 0; t < 8; ++t) {
            selecta::match::RemoteAlbumFile file;
            file.parsed = parsed.at(t);
            file.file.path = path + u'\\' + names.at(t);
            file.file.attributes.durationSec = 150 + (a * 7) + (t * 11);
            files.append(file);
        }
        return files;
    };
    const selecta::match::AlbumMatcher matcher(index); // one object, shared
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int a = t % 4; a < 20; a += 4) {
                const selecta::match::AlbumMatchResult result = matcher.match(folderOf(a));
                if (!result.best.inCollection || result.best.albumId != static_cast<quint64>(a) + 1 ||
                    result.owned != 8) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

TEST(Concurrency, ArtistDictionaryAndParserAreReentrant)
{
    const selecta::match::ArtistDictionary dictionary({{u"Bicep"_s, 5}, {u"Daft Punk"_s, 9}});
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                const ParsedRemote parsed =
                    RemotePathParser::parse(u"Glue - Bicep.mp3"_s, {}, ParseContext{&dictionary, nullptr});
                if (parsed.artist.value != u"Bicep"_s || !dictionary.lookup(u"Daft Pnk"_s).found()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

} // namespace
