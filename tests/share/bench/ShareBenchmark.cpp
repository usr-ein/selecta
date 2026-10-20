// SPDX-License-Identifier: GPL-3.0-or-later
// Share index benchmark (contract: a first build of 20 k files with knownAudio complete under about 3 s; at 100 k files
// an index under about 60 MB and a typical query under about 1 ms, on an M-series Mac).
//
//   build/<release dir>/tests/share/selecta_share_bench
//   SELECTA_BENCH_FILES (default 20000): the tree for the build benchmarks
//   SELECTA_BENCH_SEARCH_FILES (default 100000): the tree for memory, search and lookup
//   SELECTA_BENCH_KEEP=0: trees in a temporary folder removed afterwards (the ctest smoke run)
//
// The trees are synthetic: artist folders of album folders ("Artist 12 - Album 345 (2019)") of 10 tracks
// ("07 - Artist 12 - Title 3456.mp3", every fifth a FLAC) and a cover.jpg each. Files hold one byte: build() only
// stats them, and knownAudio (built here as the collection would report it) gives every track its attributes. Trees
// are written once into the temporary folder and reused by later runs (delete them to rebuild).
#include "share/ShareIndex.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <benchmark/benchmark.h>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#include <sys/qos.h>
#endif

using namespace Qt::StringLiterals;
using selecta::share::BuildResult;
using selecta::share::ShareIndex;
namespace collection = selecta::collection;
namespace slsk = selecta::slsk;

namespace {

constexpr int kTracksPerAlbum = 10;
constexpr int kAlbumsPerArtist = 10;

int knob(const char *name, int fallback)
{
    const char *value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread starts
    if (value == nullptr) {
        return fallback;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value && parsed > 0 && parsed < 10'000'000 ? static_cast<int>(parsed) : fallback;
}

bool keepTrees()
{
    const char *value = std::getenv("SELECTA_BENCH_KEEP"); // NOLINT(concurrency-mt-unsafe): read before any thread
    return value == nullptr || std::string_view(value) != "0";
}

void preferPerformanceCores()
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// The process' physical footprint in bytes (what Activity Monitor shows as Memory), or 0 elsewhere.
qint64 footprint()
{
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<qint64>(info.phys_footprint);
    }
#endif
    return 0;
}

QString trackPath(const QString &root, int i)
{
    const int album = i / kTracksPerAlbum;
    const int artist = album / kAlbumsPerArtist;
    const int track = (i % kTracksPerAlbum) + 1;
    return u"%1/Artist %2/Artist %2 - Album %3 (%4)/%5 - Artist %2 - Title %6.%7"_s.arg(root)
        .arg(artist)
        .arg(album)
        .arg(1990 + (album % 30))
        .arg(track, 2, 10, QChar(u'0'))
        .arg(i)
        .arg(i % 5 == 4 ? u"flac"_s : u"mp3"_s);
}

struct BenchTree
{
    QString root;
    QHash<QString, collection::ScannedFile> known;
    int files = 0;
};

// The tree of `files` tracks (built once per size, or in a throwaway folder) and what the collection says about it.
BenchTree makeTree(int files, std::unique_ptr<QTemporaryDir> &throwaway)
{
    BenchTree tree;
    tree.files = files;
    tree.root = QDir::temp().filePath(u"selecta-share-bench-%1-v1"_s.arg(files));
    if (!keepTrees()) {
        throwaway = std::make_unique<QTemporaryDir>();
        tree.root = throwaway->filePath(u"tree"_s);
    }
    const QString marker = tree.root + u"/.complete"_s;
    const bool built = QFileInfo::exists(marker);
    if (!built) {
        QDir(tree.root).removeRecursively();
    }
    for (int i = 0; i < files; ++i) {
        const QString path = trackPath(tree.root, i);
        if (!built) {
            if (i % kTracksPerAlbum == 0) {
                const QString folder = QFileInfo(path).path();
                QDir().mkpath(folder);
                QFile cover(folder + u"/cover.jpg"_s);
                if (!cover.open(QIODevice::WriteOnly) || cover.write("j", 1) != 1) {
                    return {};
                }
            }
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write("a", 1) != 1) {
                return {};
            }
        }
        struct stat status{};
        if (::stat(QFile::encodeName(path).constData(), &status) != 0) {
            return {};
        }
        collection::ScannedFile scanned;
        scanned.file = {path, static_cast<qint64>(status.st_size),
                        (static_cast<qint64>(status.st_mtimespec.tv_sec) * 1000) +
                            (status.st_mtimespec.tv_nsec / 1'000'000)};
        const bool flac = path.endsWith(u".flac"_s);
        scanned.quality = flac ? selecta::match::AudioQuality{selecta::match::Codec::Flac, 1000, {}, 44100, 16}
                               : selecta::match::AudioQuality{selecta::match::Codec::Mp3, 320, false, 44100, 0};
        scanned.durationMs = 180'000 + (i % 240) * 1000;
        tree.known.insert(path, scanned);
    }
    if (!built) {
        QFile done(marker);
        if (!done.open(QIODevice::WriteOnly)) {
            return {};
        }
    }
    return tree;
}

const BenchTree &buildTree()
{
    static std::unique_ptr<QTemporaryDir> throwaway;
    static const BenchTree tree = makeTree(knob("SELECTA_BENCH_FILES", 20000), throwaway);
    return tree;
}

const BenchTree &searchTree()
{
    static std::unique_ptr<QTemporaryDir> throwaway;
    static const BenchTree tree = makeTree(knob("SELECTA_BENCH_SEARCH_FILES", 100000), throwaway);
    return tree;
}

BuildResult buildOf(const BenchTree &tree)
{
    return selecta::share::build({{tree.root, u"Music"_s}}, tree.known);
}

// The 100 k index, built once, with what building it cost.
struct SearchIndex
{
    std::shared_ptr<const ShareIndex> index;
    qint64 footprintGrowth = 0;
    double seconds = 0;
};

const SearchIndex &searchIndex()
{
    static const SearchIndex built = [] {
        SearchIndex result;
        const BenchTree &tree = searchTree();
        if (tree.root.isEmpty()) {
            return result;
        }
        const qint64 before = footprint();
        QElapsedTimer timer;
        timer.start();
        result.index = buildOf(tree).index;
        result.seconds = static_cast<double>(timer.elapsed()) / 1000.0;
        result.footprintGrowth = footprint() - before;
        return result;
    }();
    return built;
}

void BM_FirstBuild(benchmark::State &state)
{
    preferPerformanceCores();
    const BenchTree &tree = buildTree();
    if (tree.root.isEmpty()) {
        state.SkipWithError("can't build the benchmark tree");
        return;
    }
    BuildResult result;
    QElapsedTimer timer;
    qint64 elapsedMs = 0;
    for (auto _ : state) {
        timer.start();
        result = buildOf(tree);
        elapsedMs = timer.elapsed();
        benchmark::DoNotOptimize(result);
    }
    state.counters["files"] = result.index->counts().files;
    state.counters["folders"] = result.index->counts().folders;
    state.counters["seconds"] = static_cast<double>(elapsedMs) / 1000.0;
    state.counters["index_MB"] = static_cast<double>(result.index->memoryBytes()) / 1e6;
    state.counters["list_KB"] = static_cast<double>(result.index->sharedFileList().size()) / 1e3;
}
BENCHMARK(BM_FirstBuild)->Unit(benchmark::kMillisecond)->Iterations(3);

void BM_IndexMemory(benchmark::State &state)
{
    const SearchIndex &built = searchIndex();
    if (!built.index) {
        state.SkipWithError("can't build the search tree");
        return;
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(built.index->counts());
    }
    state.counters["files"] = built.index->counts().files;
    state.counters["build_s"] = built.seconds;
    state.counters["index_MB"] = static_cast<double>(built.index->memoryBytes()) / 1e6;
    state.counters["footprint_growth_MB"] = static_cast<double>(built.footprintGrowth) / 1e6;
    state.counters["list_MB"] = static_cast<double>(built.index->sharedFileList().size()) / 1e6;
}
BENCHMARK(BM_IndexMemory)->Iterations(1);

void searchBenchmark(benchmark::State &state, const QStringList &queries)
{
    const SearchIndex &built = searchIndex();
    if (!built.index) {
        state.SkipWithError("can't build the search tree");
        return;
    }
    const QStringList phrases{u"excluded phrase one"_s, u"another banned phrase"_s};
    qsizetype i = 0;
    qint64 results = 0;
    for (auto _ : state) {
        QList<slsk::FileEntry> found = built.index->search(queries.at(i % queries.size()), phrases);
        results += found.size();
        benchmark::DoNotOptimize(found);
        ++i;
    }
    state.counters["results_per_query"] = static_cast<double>(results) / static_cast<double>(std::max<qsizetype>(i, 1));
}

void BM_SearchTypical(benchmark::State &state)
{
    searchBenchmark(state, {u"artist 42 title"_s, u"artist 7 album 71"_s, u"title 4242"_s, u"artist 99 flac"_s,
                            u"album 512 cover"_s, u"artist 3 - album 31 (2001)"_s, u"nothing matches this"_s,
                            u"title 12345 mp3"_s});
}
BENCHMARK(BM_SearchTypical)->Unit(benchmark::kMicrosecond);

void BM_SearchBroad(benchmark::State &state)
{
    // Every file matches: the cap stops the walk.
    searchBenchmark(state, {u"music"_s, u"artist mp3"_s, u"title -flac"_s});
}
BENCHMARK(BM_SearchBroad)->Unit(benchmark::kMicrosecond);

void BM_SearchSuffix(benchmark::State &state)
{
    searchBenchmark(state, {u"artist 42 *itle"_s, u"album *99"_s, u"title *3 -mp3"_s});
}
BENCHMARK(BM_SearchSuffix)->Unit(benchmark::kMicrosecond);

void BM_Lookup(benchmark::State &state)
{
    const SearchIndex &built = searchIndex();
    if (!built.index) {
        state.SkipWithError("can't build the search tree");
        return;
    }
    const int files = searchTree().files;
    QList<slsk::RemotePath> paths;
    for (int i = 0; i < 64; ++i) {
        const int n = (i * 7919) % files;
        QString path = trackPath(u"Music"_s, n);
        path.replace(u'/', u'\\');
        paths.append(slsk::RemotePath::fromText(state.range(0) == 0 ? path : path.toLower()));
    }
    qsizetype i = 0;
    qint64 found = 0;
    for (auto _ : state) {
        found += built.index->lookup(paths.at(i % paths.size())).has_value() ? 1 : 0;
        ++i;
    }
    state.counters["found"] = static_cast<double>(found) / static_cast<double>(std::max<qsizetype>(i, 1));
}
BENCHMARK(BM_Lookup)->Arg(0)->Arg(1)->Unit(benchmark::kMicrosecond); // exact, then lower-cased (the fallback)

void BM_FolderContents(benchmark::State &state)
{
    const SearchIndex &built = searchIndex();
    if (!built.index) {
        state.SkipWithError("can't build the search tree");
        return;
    }
    QString folder = QFileInfo(trackPath(u"Music"_s, searchTree().files / 2)).path();
    folder.replace(u'/', u'\\');
    const slsk::RemotePath path = slsk::RemotePath::fromText(folder);
    for (auto _ : state) {
        benchmark::DoNotOptimize(built.index->folderContents(path));
    }
}
BENCHMARK(BM_FolderContents)->Unit(benchmark::kMicrosecond);

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
