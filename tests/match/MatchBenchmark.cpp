// SPDX-License-Identifier: GPL-3.0-or-later
// Match library benchmark (contract: a synthetic 200 k-track library; build time, memory, µs per result, p95 < 100 µs).
//
//   build/<release dir>/tests/match/selecta_match_benchmark          (SELECTA_BENCH_TRACKS overrides 200000)
//
// The library and the remote results are synthetic and deterministic: a pronounceable vocabulary, Zipf-like artists
// (a few prolific, many with one track), versions, albums, and remote names that are exact copies, typos, other
// layouts or not owned at all. Latencies are reported on the wall clock and on the thread's CPU clock: the second
// leaves out time spent waiting for a core, so it stays meaningful on a busy machine.
#include "match/MatchIndex.h"
#include "match/Matcher.h"
#include "match/ReleaseScorer.h"
#include "match/RemotePathParser.h"

#include <QString>
#include <QStringList>

#include <benchmark/benchmark.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first: psapi.h needs its types.
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::match::LibraryAlbum;
using selecta::match::LibraryTrack;
using selecta::match::Matcher;
using selecta::match::MatchIndex;
using selecta::match::MatchIndexInput;
using selecta::match::ParseContext;
using selecta::match::ParsedRemote;
using selecta::match::ReleaseReference;
using selecta::match::ReleaseScorer;
using selecta::match::RemoteFile;
using selecta::match::RemotePathParser;

namespace {

class Random
{
public:
    explicit Random(std::uint64_t seed)
        : m_state(seed)
    {}
    std::uint64_t next()
    {
        m_state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    int below(int bound) { return static_cast<int>(next() % static_cast<std::uint64_t>(bound)); }
    double unit() { return static_cast<double>(next() >> 11U) / static_cast<double>(1ULL << 53U); }

private:
    std::uint64_t m_state;
};

QString word(Random &random)
{
    static const QStringList onsets{u"b"_s, u"d"_s, u"f"_s, u"g"_s, u"k"_s,  u"l"_s,  u"m"_s,  u"n"_s,  u"p"_s, u"r"_s,
                                    u"s"_s, u"t"_s, u"v"_s, u"z"_s, u"br"_s, u"tr"_s, u"st"_s, u"ch"_s, u"sh"_s};
    static const QStringList vowels{u"a"_s, u"e"_s, u"i"_s, u"o"_s, u"u"_s, u"ai"_s, u"ou"_s, u"ea"_s};
    QString text;
    const int syllables = 1 + random.below(3);
    for (int s = 0; s < syllables; ++s) {
        text += onsets.at(random.below(static_cast<int>(onsets.size()))) +
                vowels.at(random.below(static_cast<int>(vowels.size())));
    }
    if (random.below(3) == 0) {
        text += u"n"_s;
    }
    text[0] = text.at(0).toUpper();
    return text;
}

QString phrase(Random &random, const QStringList &vocabulary, int minWords, int maxWords)
{
    QStringList words;
    const int count = minWords + random.below(maxWords - minWords + 1);
    for (int i = 0; i < count; ++i) {
        // Zipf-like word choice: frequent words are frequent.
        const double u = random.unit();
        const auto index = static_cast<qsizetype>(std::pow(u, 2.5) * static_cast<double>(vocabulary.size()));
        words.append(vocabulary.at(std::min(index, vocabulary.size() - 1)));
    }
    return words.join(u' ');
}

QString typo(Random &random, const QString &text)
{
    QString out = text;
    if (out.size() < 6) {
        return out;
    }
    const qsizetype at = 1 + random.below(static_cast<int>(out.size()) - 2);
    if (out.at(at).isSpace() || out.at(at - 1).isSpace()) {
        return out;
    }
    switch (random.below(3)) {
    case 0:
        out.remove(at, 1);
        break;
    case 1:
        out.insert(at, out.at(at));
        break;
    default:
        std::swap(out[at], out[at - 1]);
        break;
    }
    return out;
}

struct Library
{
    MatchIndexInput input;
    QStringList vocabulary;
};

Library makeLibrary(int tracks)
{
    Library library;
    Random random(20261020);
    for (int i = 0; i < 30000; ++i) {
        library.vocabulary.append(word(random));
    }
    QStringList artists;
    const int artistCount = std::max(10, tracks / 10);
    for (int i = 0; i < artistCount; ++i) {
        artists.append(phrase(random, library.vocabulary, 1, 3));
    }
    static const QStringList versions{
        u""_s, u""_s, u""_s, u" (Original Mix)"_s, u" (Extended Mix)"_s, u" (Radio Edit)"_s, u" (Dub)"_s};
    quint64 albumId = 1;
    int inAlbum = 0;
    for (int i = 0; i < tracks; ++i) {
        LibraryTrack track;
        track.id = static_cast<quint64>(i) + 1;
        const double u = random.unit();
        track.artist = artists.at(static_cast<qsizetype>(std::pow(u, 3.0) * static_cast<double>(artistCount)));
        QString version = versions.at(random.below(static_cast<int>(versions.size())));
        if (random.below(10) == 0) {
            version = u" ("_s + artists.at(random.below(artistCount)) + u" Remix)"_s;
        }
        track.title = phrase(random, library.vocabulary, 1, 4) + version;
        track.durationMs = 120000 + (static_cast<qint64>(random.below(480)) * 1000) + random.below(1000);
        track.quality = {selecta::match::Codec::Mp3, 320, false, 44100, 0};
        track.source = random.below(20) == 0 ? selecta::match::Source::Inbox : selecta::match::Source::Collection;
        if (inAlbum == 0 && random.below(3) == 0) {
            inAlbum = 8 + random.below(8);
            LibraryAlbum album;
            album.id = albumId;
            album.artist = track.artist;
            album.title = phrase(random, library.vocabulary, 1, 3);
            library.input.albums.append(album);
            ++albumId;
        }
        if (inAlbum > 0) {
            track.albumId = albumId - 1;
            track.album = library.input.albums.constLast().title;
            --inAlbum;
        }
        library.input.tracks.append(track);
    }
    library.input.generation = 1;
    return library;
}

enum class ProbeKind : quint8 {
    Copy,     // the owned track's own name
    Typo,     // one typo in the artist and one in the title
    Layout,   // artist folder, album folder, numbered title
    NotOwned, // random words: should match nothing
};

struct Probe
{
    QString path;
    RemoteFile file;
    ProbeKind kind = ProbeKind::NotOwned;
    quint64 expected = 0; // the owned track's id (0 for NotOwned)
};

std::vector<Probe> makeProbes(const Library &library, int count)
{
    std::vector<Probe> probes;
    Random random(42);
    const auto &tracks = library.input.tracks;
    for (int i = 0; i < count; ++i) {
        const LibraryTrack &track = tracks.at(random.below(static_cast<int>(tracks.size())));
        Probe probe;
        const int kind = random.below(10);
        int length = static_cast<int>(track.durationMs / 1000);
        probe.expected = track.id;
        if (kind < 4) { // an exact copy
            probe.kind = ProbeKind::Copy;
            probe.path = u"Music\\"_s + track.artist + u" - "_s + track.title + u".mp3"_s;
        } else if (kind < 6) { // a typo
            probe.kind = ProbeKind::Typo;
            probe.path = u"DJ\\"_s + typo(random, track.artist) + u" - "_s + typo(random, track.title) + u".mp3"_s;
        } else if (kind < 7) { // folder layout
            probe.kind = ProbeKind::Layout;
            probe.path = u"Share\\"_s + track.artist + u"\\Album (2019)\\0"_s + QString::number(1 + random.below(9)) +
                         u" - "_s + track.title + u".flac"_s;
        } else { // not owned
            probe.kind = ProbeKind::NotOwned;
            probe.expected = 0;
            probe.path = u"Incoming\\"_s + phrase(random, library.vocabulary, 1, 2) + u" - "_s +
                         phrase(random, library.vocabulary, 1, 4) + u".mp3"_s;
            length = 120 + random.below(480);
        }
        probe.file.path = probe.path;
        probe.file.size = 9'000'000;
        probe.file.attributes.durationSec = length;
        probe.file.attributes.bitrateKbps = 320;
        probes.push_back(std::move(probe));
    }
    return probes;
}

// Benchmark knobs: SELECTA_BENCH_TRACKS (library size, 200000) and SELECTA_BENCH_RESULTS (results timed, 10000).
int knob(const char *name, int fallback)
{
    const char *value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread starts
    if (value == nullptr) {
        return fallback;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value && parsed > 0 && parsed < 100'000'000 ? static_cast<int>(parsed) : fallback;
}

bool diagnose()
{
    return knob("SELECTA_BENCH_DIAGNOSE", 0) > 0;
}

int trackCount()
{
    return std::max(100, knob("SELECTA_BENCH_TRACKS", 200000));
}

int resultCount()
{
    return std::max(100, knob("SELECTA_BENCH_RESULTS", 10000));
}

const Library &library()
{
    static const Library value = makeLibrary(trackCount());
    return value;
}

std::shared_ptr<const MatchIndex> sharedIndex()
{
    static const std::shared_ptr<const MatchIndex> index = MatchIndex::build(library().input);
    return index;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}

// The calling thread's CPU time in µs.
double threadCpuMicros()
{
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
    const auto ticks = [](FILETIME time) { // 100 ns each
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10.0;
#else
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (static_cast<double>(now.tv_sec) * 1e6) + (static_cast<double>(now.tv_nsec) / 1e3);
#endif
}

// On Apple silicon, ask for a performance core: the efficiency cores would measure the scheduler, not the matcher.
void preferPerformanceCores()
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

long maxResidentKilobytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0;
    }
    return static_cast<long>(counters.PeakWorkingSetSize / 1024);
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    // glibc declares ru_maxrss in an anonymous union with a padding word.
    const long peak = usage.ru_maxrss; // NOLINT(cppcoreguidelines-pro-type-union-access)
#ifdef __APPLE__
    return peak / 1024; // bytes on macOS
#else
    return peak; // kilobytes on Linux
#endif
#endif
}

void BM_BuildIndex(benchmark::State &state)
{
    preferPerformanceCores();
    const Library &input = library();
    const long before = maxResidentKilobytes();
    std::size_t memory = 0;
    while (state.KeepRunning()) {
        std::shared_ptr<const MatchIndex> index = MatchIndex::build(input.input);
        memory = index->memoryUsage();
        benchmark::DoNotOptimize(index);
    }
    state.counters["tracks"] = static_cast<double>(input.input.tracks.size());
    state.counters["index_MB"] = static_cast<double>(memory) / (1024.0 * 1024.0);
    state.counters["rss_growth_MB"] = static_cast<double>(maxResidentKilobytes() - before) / 1024.0;
}
BENCHMARK(BM_BuildIndex)->Unit(benchmark::kMillisecond)->Iterations(1);

// Parse and match one result at a time; per-result latency percentiles as counters.
void BM_MatchResult(benchmark::State &state)
{
    preferPerformanceCores();
    const std::shared_ptr<const MatchIndex> index = sharedIndex();
    const Matcher matcher(index);
    const std::vector<Probe> probes = makeProbes(library(), resultCount());
    std::vector<double> parseMicros;
    std::vector<double> matchMicros;
    std::vector<double> totalMicros;
    std::vector<double> matchCpuMicros;
    std::vector<double> totalCpuMicros;
    int reported = 0;
    std::array<std::size_t, 4> seen{};
    std::array<std::size_t, 4> found{}; // the expected track (or, for NotOwned, any track) at tier ≥ likely
    std::size_t next = 0;
    while (state.KeepRunning()) {
        const Probe &probe = probes[next++ % probes.size()];
        const double cpuStart = threadCpuMicros();
        const auto start = std::chrono::steady_clock::now();
        const ParsedRemote parsed = RemotePathParser::parse(probe.path, {}, ParseContext{&index->artists(), nullptr});
        const auto parsedAt = std::chrono::steady_clock::now();
        const double cpuParsedAt = threadCpuMicros();
        selecta::match::MatchResult result = matcher.match(parsed, probe.file);
        const auto end = std::chrono::steady_clock::now();
        const double cpuEnd = threadCpuMicros();
        const auto kind = static_cast<std::size_t>(probe.kind);
        ++seen[kind];
        const bool any = result.collection.isSet() || result.inbox.isSet();
        // The library may hold the same track twice (same artist, title and length): either id is right.
        const auto same = [&probe](quint64 id) {
            if (id == probe.expected) {
                return true;
            }
            const auto &tracks = library().input.tracks;
            const LibraryTrack &a = tracks.at(static_cast<qsizetype>(id - 1));
            const LibraryTrack &b = tracks.at(static_cast<qsizetype>(probe.expected - 1));
            return a.artist == b.artist && a.title == b.title && std::abs(a.durationMs - b.durationMs) <= 3000;
        };
        const bool expected = (result.collection.isSet() && same(result.collection.id)) ||
                              (result.inbox.isSet() && same(result.inbox.id));
        const bool right = probe.kind == ProbeKind::NotOwned ? any : expected;
        found[kind] += right ? 1 : 0;
        if (diagnose() && probe.kind == ProbeKind::Copy && !expected && reported < 25) {
            ++reported;
            const LibraryTrack &want = library().input.tracks.at(static_cast<qsizetype>(probe.expected - 1));
            const selecta::match::MatchResult explained = matcher.match(parsed, probe.file, {true});
            static_cast<void>(std::fprintf(stderr, "MISS %s | want %s - %s (%lld ms)\n%s\n\n", qPrintable(probe.path),
                                           qPrintable(want.artist), qPrintable(want.title),
                                           static_cast<long long>(want.durationMs),
                                           qPrintable(explained.explanation.join(u'\n'))));
        }
        parseMicros.push_back(std::chrono::duration<double, std::micro>(parsedAt - start).count());
        matchMicros.push_back(std::chrono::duration<double, std::micro>(end - parsedAt).count());
        totalMicros.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        matchCpuMicros.push_back(cpuEnd - cpuParsedAt);
        totalCpuMicros.push_back(cpuEnd - cpuStart);
        benchmark::DoNotOptimize(result);
    }
    state.counters["parse_p50_us"] = percentile(parseMicros, 0.50);
    state.counters["parse_p95_us"] = percentile(parseMicros, 0.95);
    state.counters["match_p50_us"] = percentile(matchMicros, 0.50);
    state.counters["match_p95_us"] = percentile(matchMicros, 0.95);
    state.counters["total_p50_us"] = percentile(totalMicros, 0.50);
    state.counters["total_p95_us"] = percentile(totalMicros, 0.95);
    state.counters["total_p99_us"] = percentile(totalMicros, 0.99);
    state.counters["match_cpu_p95_us"] = percentile(matchCpuMicros, 0.95);
    state.counters["total_cpu_p95_us"] = percentile(totalCpuMicros, 0.95);
    const auto share = [&seen, &found](ProbeKind kind) {
        const auto k = static_cast<std::size_t>(kind);
        return seen[k] == 0 ? 0.0 : static_cast<double>(found[k]) / static_cast<double>(seen[k]);
    };
    state.counters["recall_copy"] = share(ProbeKind::Copy);
    state.counters["recall_typo"] = share(ProbeKind::Typo);
    state.counters["recall_layout"] = share(ProbeKind::Layout);
    state.counters["false_owned"] = share(ProbeKind::NotOwned);
}
BENCHMARK(BM_MatchResult)->Unit(benchmark::kMicrosecond)->Iterations(resultCount());

// The release-guided fit of one result (Get tracks, "Only this release").
void BM_ReleaseFit(benchmark::State &state)
{
    const std::vector<Probe> probes = makeProbes(library(), 2000);
    const ReleaseScorer scorer(ReleaseReference{{u"Bicep"_s}, u"Glue (Original Mix)"_s, {}, 269000});
    std::vector<ParsedRemote> parsed;
    parsed.reserve(probes.size());
    for (const Probe &probe : probes) {
        parsed.push_back(RemotePathParser::parse(probe.path));
    }
    std::size_t next = 0;
    while (state.KeepRunning()) {
        const std::size_t i = next++ % parsed.size();
        benchmark::DoNotOptimize(scorer.score(parsed[i], probes[i].file.attributes.durationSec));
    }
}
BENCHMARK(BM_ReleaseFit)->Unit(benchmark::kMicrosecond);

} // namespace

BENCHMARK_MAIN();
