// SPDX-License-Identifier: GPL-3.0-or-later
// One ShareIndex answers from many threads at once; build() runs on several workers at once (run under TSan).
#include "share/Open.h"
#include "share/ShareIndex.h"

#include "ShareTestSupport.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::share::BuildResult;
using selecta::share::ShareIndex;
using selecta::share::test::buildTree;
using selecta::share::test::Tree;
namespace slsk = selecta::slsk;
namespace test = selecta::share::test;

namespace {

constexpr int kThreads = 8;

void fill(const Tree &tree)
{
    for (int i = 0; i < 40; ++i) {
        tree.file(u"Artist %1/Album %2/%3 Track.mp3"_s.arg(i % 4).arg(i % 8).arg(i),
                  i % 10 == 0 ? test::mp3Cbr320() : QByteArrayLiteral("data"));
    }
}

TEST(ShareConcurrency, ManyReadersShareOneIndex)
{
    Tree tree;
    fill(tree);
    const std::shared_ptr<const ShareIndex> index = buildTree(tree).index;
    ASSERT_EQ(index->counts().files, 40);
    const slsk::RemotePath path = slsk::RemotePath::fromText(u"Music\\Artist 1\\Album 1\\1 Track.mp3"_s);
    const slsk::RemotePath lower = slsk::RemotePath::fromText(u"music\\artist 1\\album 1\\1 track.mp3"_s);
    const slsk::RemotePath folder = slsk::RemotePath::fromText(u"Music\\Artist 1\\Album 1"_s);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int round = 0; round < 40; ++round) {
                const bool ok = index->search(u"artist track"_s, {u"nothing like this"_s}).size() == 40 &&
                                index->search(u"music *ack -album"_s, {}).isEmpty() &&
                                index->folderContents(folder).size() == 1 && !index->sharedFileList().isEmpty() &&
                                index->counts().files == 40 && index->roots().size() == 1 &&
                                index->memoryBytes() > 0 && index->lookup(lower).has_value();
                const std::optional<selecta::share::SharedFile> file = index->lookup(path);
                const bool opened = file.has_value() && selecta::share::open(*file).ok();
                if (!ok || !opened) {
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

TEST(ShareConcurrency, BuildsRunSideBySide)
{
    Tree tree;
    fill(tree);
    const QString track = tree.path(u"Artist 0/Album 0/0 Track.mp3"_s);
    const QHash<QString, selecta::collection::ScannedFile> known{
        {track, test::scanned(track, {selecta::match::Codec::Mp3, 256, false, 44100, 0}, 2'000)}};
    std::vector<BuildResult> results(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t t = 0; t < results.size(); ++t) {
        threads.emplace_back([&results, &tree, &known, t] { results[t] = buildTree(tree, known); });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    for (const BuildResult &result : results) {
        ASSERT_NE(result.index, nullptr);
        EXPECT_FALSE(result.cancelled);
        EXPECT_EQ(result.index->counts(), results.front().index->counts());
        EXPECT_EQ(result.index->sharedFileList(), results.front().index->sharedFileList());
    }
    EXPECT_EQ(results.front().index->counts().files, 40);
}

} // namespace
