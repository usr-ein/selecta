// SPDX-License-Identifier: GPL-3.0-or-later
// Collection scanner benchmark (contract: a first scan of 20 k files under about 60 s, a rescan with nothing changed
// under about 2 s, on an M-series Mac).
//
//   build/<release dir>/tests/collection/selecta_collection_bench          (SELECTA_BENCH_FILES overrides 20000)
//
// SELECTA_BENCH_KEEP=0 builds the tree in a temporary folder that is removed afterwards (the ctest smoke run).
//
// The tree is synthetic: artist folders of album folders of tracks, 70 % tagged MP3 320 with a 16 KB ID3 block (the
// size of a small cover), 20 % tagged FLAC, 10 % untagged MP3 named "NN - Artist - Title". It is written once into
// the temporary folder and reused by later runs (delete it to rebuild). Every file has blocks of its own (written, not
// cloned), but the page cache is warm after writing them: numbers are for a warm cache, and a scan reads only each
// file's header and tags (tens of KB), so a cold SSD adds little.
#include "SyntheticAudio.h"
#include "collection/Albums.h"
#include "collection/FolderRoles.h"
#include "collection/Scanner.h"
#include "convert/TagWriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <benchmark/benchmark.h>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <QTemporaryDir>

#include <cstdlib>
#include <memory>
#include <string_view>

using namespace Qt::StringLiterals;
using selecta::collection::FileSignature;
using selecta::collection::ScanOptions;
using selecta::collection::ScanResult;

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

// SELECTA_BENCH_KEEP=0: a throwaway tree, removed at exit.
bool keepTree()
{
    const char *value = std::getenv("SELECTA_BENCH_KEEP"); // NOLINT(concurrency-mt-unsafe): read before any thread
    return value == nullptr || std::string_view(value) != "0";
}

int fileCount()
{
    return knob("SELECTA_BENCH_FILES", 20000);
}

void preferPerformanceCores()
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

QByteArray tagged(const QByteArray &audio, const QString &extension, const QString &artist, const QString &title)
{
    const QString path =
        QDir::temp().filePath(u"selecta-bench-tag-%1.%2"_s.arg(QCoreApplication::applicationPid()).arg(extension));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(audio) != audio.size()) {
        return {};
    }
    file.close();
    selecta::convert::MetadataModel model;
    model.set(selecta::convert::keys::Artist, artist, selecta::convert::Provenance::User);
    model.set(selecta::convert::keys::Title, title, selecta::convert::Provenance::User);
    model.set(selecta::convert::keys::Album, u"Album"_s, selecta::convert::Provenance::User);
    model.set(selecta::convert::keys::TrackNumber, u"1/10"_s, selecta::convert::Provenance::User);
    model.set(selecta::convert::keys::Date, u"2021"_s, selecta::convert::Provenance::User);
    if (!selecta::convert::writeTags(path, model).ok() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray bytes = file.readAll();
    file.close();
    QFile::remove(path);
    return bytes;
}

// The benchmark tree under the temporary folder, built once per file count (or in a throwaway folder).
QString tree()
{
    static std::unique_ptr<QTemporaryDir> throwaway;
    static const QString root = [] {
        const int files = fileCount();
        QString folder = QDir::temp().filePath(u"selecta-collection-bench-%1-v1"_s.arg(files));
        if (!keepTree()) {
            throwaway = std::make_unique<QTemporaryDir>();
            folder = throwaway->filePath(u"tree"_s);
        }
        const QString marker = folder + u"/.complete"_s;
        if (QFileInfo::exists(marker)) {
            return folder;
        }
        QDir(folder).removeRecursively();
        QDir().mkpath(folder);
        const selecta::testing::Pcm pcm = selecta::testing::tones({440.0, 1250.0}, 1.0);
        const QByteArray mp3 = tagged(selecta::testing::makeMp3(pcm, {.id3v2Bytes = 16 * 1024}), u"mp3"_s, u"Artist"_s,
                                      u"Title (Original Mix)"_s);
        const QByteArray flac = tagged(selecta::testing::makeFlac(selecta::testing::quantised(pcm, 16)), u"flac"_s,
                                       u"Artist"_s, u"Title (Extended Mix)"_s);
        const QByteArray untagged = selecta::testing::makeMp3(pcm);
        for (int i = 0; i < files; ++i) {
            const int album = i / kTracksPerAlbum;
            const int artist = album / kAlbumsPerArtist;
            const int track = (i % kTracksPerAlbum) + 1;
            const QString albumFolder =
                u"%1/Artist %2/Artist %2 - Album %3 (2021)"_s.arg(folder).arg(artist).arg(album);
            if (track == 1) {
                QDir().mkpath(albumFolder);
            }
            const int kind = i % 10;
            QString name;
            const QByteArray *bytes = &mp3;
            if (kind < 7) {
                name = u"%1 - Title %2.mp3"_s.arg(track, 2, 10, QChar(u'0')).arg(i);
            } else if (kind < 9) {
                name = u"%1 - Title %2.flac"_s.arg(track, 2, 10, QChar(u'0')).arg(i);
                bytes = &flac;
            } else {
                name = u"%1 - Artist %2 - Title %3.mp3"_s.arg(track, 2, 10, QChar(u'0')).arg(artist).arg(i);
                bytes = &untagged;
            }
            QFile file(albumFolder + u'/' + name);
            if (!file.open(QIODevice::WriteOnly) || file.write(*bytes) != bytes->size()) {
                return QString();
            }
        }
        QFile done(marker);
        if (!done.open(QIODevice::WriteOnly)) {
            return QString();
        }
        return folder;
    }();
    return root;
}

QHash<QString, FileSignature> signaturesOf(const ScanResult &result)
{
    QHash<QString, FileSignature> known;
    for (const auto &file : result.added) {
        known.insert(file.file.path, file.file);
    }
    return known;
}

void BM_FirstScan(benchmark::State &state)
{
    preferPerformanceCores();
    const QString root = tree();
    if (root.isEmpty()) {
        state.SkipWithError("can't build the benchmark tree");
        return;
    }
    ScanOptions options;
    options.readers = static_cast<int>(state.range(0));
    ScanResult result;
    QElapsedTimer timer;
    qint64 elapsedMs = 0;
    while (state.KeepRunning()) {
        timer.start();
        result = selecta::collection::scan({root}, {}, options);
        elapsedMs = timer.elapsed();
        benchmark::DoNotOptimize(result);
    }
    QHash<QString, quint64> albumIds;
    const auto albums = selecta::collection::groupAlbums(result.added, &albumIds);
    state.counters["files"] = static_cast<double>(result.added.size());
    state.counters["unreadable"] = static_cast<double>(result.unreadable.size());
    state.counters["albums"] = static_cast<double>(albums.size());
    state.counters["seconds"] = static_cast<double>(elapsedMs) / 1000.0;
    state.counters["files_per_s"] =
        elapsedMs > 0 ? static_cast<double>(result.added.size()) * 1000.0 / static_cast<double>(elapsedMs) : 0.0;
}
BENCHMARK(BM_FirstScan)->Arg(4)->Arg(8)->Unit(benchmark::kMillisecond)->Iterations(1);

void BM_Rescan(benchmark::State &state)
{
    preferPerformanceCores();
    const QString root = tree();
    if (root.isEmpty()) {
        state.SkipWithError("can't build the benchmark tree");
        return;
    }
    const QHash<QString, FileSignature> known = signaturesOf(selecta::collection::scan({root}, {}));
    ScanResult result;
    while (state.KeepRunning()) {
        result = selecta::collection::scan({root}, known);
        benchmark::DoNotOptimize(result);
    }
    state.counters["files"] = static_cast<double>(known.size());
    state.counters["read"] = static_cast<double>(result.added.size() + result.changed.size());
    state.counters["removed"] = static_cast<double>(result.removed.size());
}
BENCHMARK(BM_Rescan)->Unit(benchmark::kMillisecond)->Iterations(3);

void BM_GroupAlbums(benchmark::State &state)
{
    const QString root = tree();
    if (root.isEmpty()) {
        state.SkipWithError("can't build the benchmark tree");
        return;
    }
    const ScanResult scanned = selecta::collection::scan({root}, {});
    while (state.KeepRunning()) {
        QHash<QString, quint64> ids;
        auto albums = selecta::collection::groupAlbums(scanned.added, &ids);
        benchmark::DoNotOptimize(albums);
    }
}
BENCHMARK(BM_GroupAlbums)->Unit(benchmark::kMillisecond)->Iterations(3);

// The app's grouping (PRD LIB-1): one album per subfolder of the albums folder, here the tree's artist folders.
void BM_AlbumsOf(benchmark::State &state)
{
    const QString root = tree();
    if (root.isEmpty()) {
        state.SkipWithError("can't build the benchmark tree");
        return;
    }
    const ScanResult scanned = selecta::collection::scan({root}, {});
    const selecta::collection::FolderRoles roles({}, root);
    qsizetype albumCount = 0;
    while (state.KeepRunning()) {
        QHash<QString, quint64> ids;
        auto albums = selecta::collection::albumsOf(scanned.added, roles, &ids);
        albumCount = albums.size();
        benchmark::DoNotOptimize(albums);
    }
    state.counters["albums"] = static_cast<double>(albumCount);
}
BENCHMARK(BM_AlbumsOf)->Unit(benchmark::kMillisecond)->Iterations(3);

} // namespace

int main(int argc, char **argv)
{
    // Qt's file name conversions and QDir need an application object for the temporary path's locale only.
    const QCoreApplication app(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
