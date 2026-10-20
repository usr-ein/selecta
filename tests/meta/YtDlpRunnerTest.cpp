// SPDX-License-Identifier: GPL-3.0-or-later
// YtDlpRunner against the fake yt-dlp script (tests/meta/fixtures/fake-yt-dlp). The real yt-dlp never runs here.
#include "meta/YtDlpRunner.h"

#include "meta/detail/ServiceGate.h"
#include "meta/support/Await.h"
#include "meta/support/FakePacer.h"
#include "meta/support/Fixtures.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>

#include <cerrno>
#include <csignal>
#include <gtest/gtest.h>
#include <memory>
#include <sys/types.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
using selecta::base::ManualClock;
using selecta::governor::Operation;
using selecta::meta::testing::awaitAdvancing;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::fixturePath;
using selecta::meta::testing::spinUntil;

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
}

// What a fetch job reported.
struct JobEvents
{
    QStringList started;
    QList<std::pair<qint64, qint64>> progress;
    std::optional<Result<FetchedAudio>> finished;
};

class YtDlpRunnerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(QDir(dir.path()).mkpath(u"bin"_s));
        ASSERT_TRUE(QDir(dir.path()).mkpath(u"cache"_s));
        writeWrapper(u""_s);
        config.executable = bin(u"yt-dlp"_s);
        config.searchDirectories = {dir.filePath(u"bin"_s)}; // Deno only when a test puts one there
        config.versionTimeout = 5s;
        config.searchTimeout = 10s;
        config.idleTimeout = 3s;
        config.totalTimeout = 10s;
        config.today = [] { return QDate(2026, 10, 20); };
    }

    void TearDown() override
    {
        m_runner.reset();
        QCoreApplication::processEvents();
    }

    QString bin(const QString &name) const { return dir.filePath(u"bin/"_s + name); }
    QString cache() const { return dir.filePath(u"cache"_s); }
    QString canonical(const QString &path) const { return QFileInfo(path).canonicalFilePath(); }

    // The executable the runner sees: a wrapper that sets the fake's variables and runs it with /bin/sh.
    void writeWrapper(const QString &extraLine)
    {
        const QString script =
            u"#!/bin/sh\n"
            u"%4\n"
            u"FAKE_YTDLP_MODE=\"$(cat '%1/mode' 2>/dev/null || echo ok)\"\n"
            u"FAKE_YTDLP_VERSION=\"$(cat '%1/version' 2>/dev/null || echo 2026.08.19)\"\n"
            u"FAKE_YTDLP_SEARCH='%2'\n"
            u"FAKE_YTDLP_ARGS='%1/args.txt'\n"
            u"FAKE_YTDLP_LOG='%1/runs.log'\n"
            u"FAKE_YTDLP_PIDFILE='%1/child.pid'\n"
            u"export FAKE_YTDLP_MODE FAKE_YTDLP_VERSION FAKE_YTDLP_SEARCH FAKE_YTDLP_ARGS "
            u"FAKE_YTDLP_LOG FAKE_YTDLP_PIDFILE\n"
            u"exec /bin/sh '%3' \"$@\"\n"_s.arg(dir.path(), fixturePath(u"youtube/search_glass_harbour.json"_s),
                                                fixturePath(u"fake-yt-dlp"_s), extraLine);
        writeFile(bin(u"yt-dlp"_s), script.toUtf8());
        QFile::setPermissions(bin(u"yt-dlp"_s), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    void setMode(const QString &mode) { writeFile(dir.filePath(u"mode"_s), mode.toUtf8()); }
    void setVersion(const QString &version) { writeFile(dir.filePath(u"version"_s), version.toUtf8()); }
    QStringList arguments() const { return readLines(dir.filePath(u"args.txt"_s)); }
    QStringList runs() const { return readLines(dir.filePath(u"runs.log"_s)); }

    void addDeno()
    {
        writeFile(bin(u"deno"_s), "#!/bin/sh\nexit 0\n");
        QFile::setPermissions(bin(u"deno"_s), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    YtDlpRunner &runner()
    {
        if (!m_runner) {
            m_runner = std::make_unique<YtDlpRunner>(config, pacer, clock);
        }
        return *m_runner;
    }

    template <typename T>
    std::optional<T> wait(const QFuture<T> &future, int ms = 15000)
    {
        return awaitAdvancing(future, clock, 1s, ms);
    }

    // Runs the event loop, moving the clock (gate spacing), until `done()`.
    bool spinAdvancing(const std::function<bool()> &done, int ms = 15000)
    {
        return spinUntil(
            [&] {
                clock.advance(1s);
                detail::ServiceGate::pollAll();
                return done();
            },
            ms);
    }

    std::shared_ptr<JobEvents> watch(YtDlpFetch *job)
    {
        auto events = std::make_shared<JobEvents>();
        QObject::connect(job, &YtDlpFetch::started, job, [events](const QString &path) { events->started << path; });
        QObject::connect(job, &YtDlpFetch::progress, job,
                         [events](qint64 done, qint64 total) { events->progress.append({done, total}); });
        QObject::connect(job, &YtDlpFetch::finished, job,
                         [events](const Result<FetchedAudio> &result) { events->finished = result; });
        return events;
    }

    // The background child the fake started in a hang mode is gone: the kill reached the whole process group.
    bool childGone()
    {
        bool hasPid = false;
        if (!spinUntil([&] { return QFileInfo::exists(dir.filePath(u"child.pid"_s)); }, 3000)) {
            return false;
        }
        const QStringList pidLines = readLines(dir.filePath(u"child.pid"_s));
        const pid_t pid = pidLines.isEmpty() ? 0 : static_cast<pid_t>(pidLines.first().toInt(&hasPid));
        if (!hasPid || pid <= 0) {
            return false;
        }
        return spinUntil([pid] { return ::kill(pid, 0) != 0 && errno == ESRCH; }, 5000);
    }

    QTemporaryDir dir;
    ManualClock clock;
    FakePacer pacer;
    YtDlpRunner::Config config;
    std::unique_ptr<YtDlpRunner> m_runner;
};

} // namespace

// --- discovery and the version check --------------------------------------------------------------------------------

TEST_F(YtDlpRunnerTest, ProbeFindsTheToolAndItsVersionWithoutAPermit)
{
    const auto result = wait(runner().probe());
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    const YtDlpInstall &install = result->value();
    EXPECT_EQ(install.path, canonical(bin(u"yt-dlp"_s)));
    EXPECT_EQ(install.version, u"2026.08.19"_s);
    EXPECT_EQ(install.versionDate, QDate(2026, 8, 19));
    EXPECT_TRUE(install.denoPath.isEmpty());
    EXPECT_FALSE(install.stale);
    EXPECT_TRUE(arguments().contains(u"--version"_s));
    EXPECT_TRUE(arguments().contains(u"--ignore-config"_s));
    EXPECT_EQ(pacer.acquiredCount(), 0); // --version is local
}

TEST_F(YtDlpRunnerTest, OldAndStaleVersions)
{
    setVersion(u"2026.05.01"_s);
    const auto stale = wait(runner().probe());
    ASSERT_TRUE(stale.has_value() && stale->ok());
    EXPECT_TRUE(stale->value().stale);
    setVersion(u"2025.07.21"_s);
    const auto old = wait(runner().probe());
    ASSERT_TRUE(old.has_value());
    EXPECT_EQ(old->error().code, ErrorCode::ToolTooOld);
    EXPECT_TRUE(old->error().message.contains(u"2025-07-21"_s));
    EXPECT_TRUE(old->error().message.contains(u"brew upgrade yt-dlp"_s));
}

TEST_F(YtDlpRunnerTest, VersionCheckFailures)
{
    setMode(u"badoption"_s); // a yt-dlp that doesn't know the safety flags
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolTooOld);
    setMode(u"versiongarbled"_s);
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolFailed);
    setMode(u"versioncrash"_s);
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolFailed);
}

TEST_F(YtDlpRunnerTest, AHangingVersionCheckTimesOutAndIsKilled)
{
    config.versionTimeout = 300ms;
    setMode(u"versionhang"_s);
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::Timeout);
    EXPECT_TRUE(childGone());
}

TEST_F(YtDlpRunnerTest, MissingAndUnsafeExecutables)
{
    config.executable = dir.filePath(u"bin/nothing-here"_s);
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolNotFound);
    runner().setExecutable(u"bin/yt-dlp"_s); // relative: never looked up in the working directory
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolNotFound);
    runner().setExecutable(bin(u"yt-dlp"_s));
    QFile::setPermissions(bin(u"yt-dlp"_s), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther |
                                                QFile::WriteOther | QFile::ExeOther);
    const auto unsafe = wait(runner().probe());
    EXPECT_EQ(unsafe->error().code, ErrorCode::ToolUnsafe);
    EXPECT_TRUE(runs().isEmpty()); // nothing ran
    QFile::setPermissions(bin(u"yt-dlp"_s), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    writeFile(bin(u"notexec"_s), "#!/bin/sh\n");
    runner().setExecutable(bin(u"notexec"_s));
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolNotFound);
}

TEST_F(YtDlpRunnerTest, DiscoveryLooksInTheDirectoriesInOrderAndResolvesSymlinks)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(u"empty"_s));
    ASSERT_TRUE(QDir(dir.path()).mkpath(u"links"_s));
    ASSERT_TRUE(QFile::link(bin(u"yt-dlp"_s), dir.filePath(u"links/yt-dlp"_s)));
    config.executable.clear();
    config.searchDirectories = {dir.filePath(u"empty"_s), dir.filePath(u"links"_s), dir.filePath(u"bin"_s)};
    const auto found = wait(runner().probe());
    ASSERT_TRUE(found.has_value() && found->ok());
    EXPECT_EQ(found->value().path, canonical(bin(u"yt-dlp"_s)));
}

TEST_F(YtDlpRunnerTest, NoPathAndNoDirectoriesMeansNoYtDlp)
{
    // What core does in isolated runs: nothing is found, nothing runs.
    config.executable.clear();
    config.searchDirectories.clear();
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::ToolNotFound);
    EXPECT_EQ(wait(runner().search(u"Kestrel Moon"_s))->error().code, ErrorCode::ToolNotFound);
    EXPECT_TRUE(runs().isEmpty());
    EXPECT_EQ(pacer.acquiredCount(), 0);
}

TEST_F(YtDlpRunnerTest, ADisabledRunnerNeverFindsOrRunsAnything)
{
    config = YtDlpRunner::Config::disabled();
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::NotConfigured);
    runner().setExecutable(bin(u"yt-dlp"_s)); // even with a path
    EXPECT_EQ(wait(runner().probe())->error().code, ErrorCode::NotConfigured);
    EXPECT_EQ(wait(runner().search(u"Kestrel Moon"_s))->error().code, ErrorCode::NotConfigured);
    const std::unique_ptr<YtDlpFetch> job(runner().fetch(u"KmTopic00Ex"_s, cache()));
    const auto events = watch(job.get());
    ASSERT_TRUE(spinUntil([&] { return events->finished.has_value(); }));
    EXPECT_EQ(events->finished->error().code, ErrorCode::NotConfigured);
    EXPECT_TRUE(runs().isEmpty());
    EXPECT_EQ(pacer.acquiredCount(), 0);
}

TEST_F(YtDlpRunnerTest, DenoIsPassedWhenFound)
{
    addDeno();
    const auto install = wait(runner().probe());
    ASSERT_TRUE(install.has_value() && install->ok());
    EXPECT_EQ(install->value().denoPath, canonical(bin(u"deno"_s)));
    ASSERT_TRUE(wait(runner().search(u"Kestrel Moon Glass Harbour"_s))->ok());
    const QStringList args = arguments();
    const qsizetype at = args.indexOf(u"--js-runtimes"_s);
    ASSERT_GE(at, 0);
    EXPECT_EQ(args.at(at + 1), QString(u"deno:"_s + canonical(bin(u"deno"_s))));
}

// --- search ---------------------------------------------------------------------------------------------------------

TEST_F(YtDlpRunnerTest, SearchReturnsCandidatesWithOnePermit)
{
    const auto result = wait(runner().search(u"Kestrel Moon  Glass Harbour\n"_s, 10));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().size(), 8);
    const QStringList args = arguments();
    EXPECT_EQ(args.last(), u"ytsearch10:Kestrel Moon Glass Harbour"_s);
    EXPECT_EQ(args.at(args.size() - 2), u"--"_s);
    EXPECT_TRUE(args.contains(u"--flat-playlist"_s));
    EXPECT_TRUE(args.contains(u"--no-cache-dir"_s));
    ASSERT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.acquired().first(), Operation::YtDlpRun);
    ASSERT_EQ(pacer.records().size(), 1);
    EXPECT_EQ(pacer.records().first().service, Service::YtDlp);
    EXPECT_EQ(pacer.records().first().method, QByteArrayLiteral("RUN"));
    EXPECT_EQ(pacer.records().first().outcome, u"ok, 8 results"_s);
    // The version is checked once, not before every run.
    ASSERT_TRUE(wait(runner().search(u"Kestrel Moon"_s, 50))->ok());
    const QStringList capped = arguments();
    EXPECT_EQ(capped.last(), u"ytsearch20:Kestrel Moon"_s); // at most 20
    EXPECT_EQ(runs().count(u"start version"_s), 1);
    EXPECT_EQ(wait(runner().search(u" \n "_s))->error().code, ErrorCode::InvalidArgument);
}

TEST_F(YtDlpRunnerTest, TheCallersEnvironmentNeverReachesYtDlp)
{
    qputenv("SELECTA_FAKE_LEAK_CANARY", "1");
    const auto result = wait(runner().search(u"Kestrel Moon"_s));
    qunsetenv("SELECTA_FAKE_LEAK_CANARY");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok()) << result->error().toString().toStdString(); // the fake exits 98 when it leaks
}

TEST_F(YtDlpRunnerTest, SearchFailures)
{
    const std::vector<std::pair<QString, ErrorCode>> table{
        {u"garbage"_s, ErrorCode::BadResponse}, {u"huge"_s, ErrorCode::TooLarge},   {u"crash"_s, ErrorCode::ToolFailed},
        {u"age"_s, ErrorCode::AgeRestricted},   {u"network"_s, ErrorCode::Network}, {u"geo"_s, ErrorCode::GeoBlocked},
        {u"oddexit"_s, ErrorCode::ToolFailed},
    };
    for (const auto &[mode, code] : table) {
        setMode(mode);
        const auto result = wait(runner().search(u"Kestrel Moon"_s));
        ASSERT_TRUE(result.has_value()) << mode.toStdString();
        EXPECT_EQ(result->error().code, code) << mode.toStdString() << " " << result->error().message.toStdString();
    }
    EXPECT_FALSE(runner().pausedUntil().has_value()); // none of these pauses previews
}

TEST_F(YtDlpRunnerTest, AHangingSearchTimesOut)
{
    config.searchTimeout = 400ms;
    setMode(u"hang"_s);
    EXPECT_EQ(wait(runner().search(u"Kestrel Moon"_s))->error().code, ErrorCode::Timeout);
    EXPECT_TRUE(childGone());
}

TEST_F(YtDlpRunnerTest, ARateLimitPausesYouTubeForAnHour)
{
    auto pausedReasons = std::make_shared<QList<ErrorCode>>();
    QObject::connect(&runner(), &YtDlpRunner::paused, &runner(),
                     [pausedReasons](const MetaError &reason) { pausedReasons->append(reason.code); });
    setMode(u"ratelimit"_s);
    const QFuture<Result<QList<YouTubeCandidate>>> first = runner().search(u"Kestrel Moon"_s);
    const QFuture<Result<QList<YouTubeCandidate>>> queued = runner().search(u"Kestrel Moon queued"_s);
    const auto limited = wait(first);
    ASSERT_TRUE(limited.has_value());
    EXPECT_EQ(limited->error().code, ErrorCode::RateLimited);
    EXPECT_EQ(limited->error().retryAfter, 3600s);
    const auto dropped = wait(queued);
    ASSERT_TRUE(dropped.has_value());
    EXPECT_EQ(dropped->error().code, ErrorCode::RateLimited); // failed without running
    ASSERT_TRUE(spinUntil([&] { return pausedReasons->size() == 1; }));
    EXPECT_EQ(pausedReasons->first(), ErrorCode::RateLimited);
    ASSERT_TRUE(runner().pausedUntil().has_value());

    setMode(u"ok"_s);
    const int runsBefore = static_cast<int>(runs().size());
    const int permitsBefore = pacer.acquiredCount();
    const auto refused = wait(runner().search(u"Kestrel Moon"_s));
    EXPECT_EQ(refused->error().code, ErrorCode::RateLimited);
    EXPECT_GT(refused->error().retryAfter.value_or(0s), 3000s);
    YtDlpFetch *job = runner().fetch(u"KmTopic00Ex"_s, cache());
    const auto events = watch(job);
    ASSERT_TRUE(spinUntil([&] { return events->finished.has_value(); }));
    EXPECT_EQ(events->finished->error().code, ErrorCode::RateLimited);
    EXPECT_EQ(static_cast<int>(runs().size()), runsBefore);
    EXPECT_EQ(pacer.acquiredCount(), permitsBefore);
    EXPECT_TRUE(wait(runner().probe())->ok()); // local: still allowed

    clock.advance(61min);
    EXPECT_FALSE(runner().pausedUntil().has_value());
    EXPECT_TRUE(wait(runner().search(u"Kestrel Moon"_s))->ok());
    delete job;
}

TEST_F(YtDlpRunnerTest, ABotCheckPausesToo)
{
    setMode(u"bot"_s);
    EXPECT_EQ(wait(runner().search(u"Kestrel Moon"_s))->error().code, ErrorCode::BotCheck);
    EXPECT_TRUE(runner().pausedUntil().has_value());
}

TEST_F(YtDlpRunnerTest, OneProcessAtATime)
{
    setMode(u"slow"_s);
    QList<QFuture<Result<QList<YouTubeCandidate>>>> searches;
    for (int i = 0; i < 3; ++i) {
        searches.append(runner().search(u"Kestrel Moon %1"_s.arg(i)));
    }
    for (const auto &search : searches) {
        const auto result = wait(search);
        ASSERT_TRUE(result.has_value() && result->ok());
    }
    const QStringList log = runs();
    ASSERT_EQ(log.size(), 8); // the version check, then three searches
    for (qsizetype i = 0; i < log.size(); ++i) {
        EXPECT_TRUE(log.at(i).startsWith(i % 2 == 0 ? u"start"_s : u"end"_s)) << log.join(u" | "_s).toStdString();
    }
    EXPECT_EQ(pacer.acquiredCount(), 3);
}

TEST_F(YtDlpRunnerTest, CancellingASearchKillsItsProcessAndTheQueueGoesOn)
{
    setMode(u"hang"_s);
    QFuture<Result<QList<YouTubeCandidate>>> hanging = runner().search(u"Kestrel Moon"_s);
    ASSERT_TRUE(spinAdvancing([&] { return QFileInfo::exists(dir.filePath(u"child.pid"_s)); }));
    hanging.cancel();
    EXPECT_TRUE(childGone());
    setMode(u"ok"_s);
    EXPECT_TRUE(wait(runner().search(u"Kestrel Moon"_s))->ok());
}

TEST_F(YtDlpRunnerTest, TheGovernorCanRefuseOrDelay)
{
    pacer.refuse(u"YouTube runs: budget spent"_s);
    const auto refused = wait(runner().search(u"Kestrel Moon"_s));
    EXPECT_EQ(refused->error().code, ErrorCode::Refused);
    EXPECT_EQ(refused->error().message, u"YouTube runs: budget spent"_s);
    EXPECT_EQ(runs().count(u"start search"_s), 0);
    pacer.refuse(u"soon"_s, 3s, 1);
    EXPECT_TRUE(wait(runner().search(u"Kestrel Moon"_s))->ok());
    EXPECT_EQ(runs().count(u"start search"_s), 1);
}

TEST_F(YtDlpRunnerTest, AChangedToolIsProbedAgain)
{
    ASSERT_TRUE(wait(runner().search(u"Kestrel Moon"_s))->ok());
    writeWrapper(u"# upgraded"_s); // brew upgrade: another file
    ASSERT_TRUE(wait(runner().search(u"Kestrel Moon"_s))->ok());
    EXPECT_EQ(runs().count(u"start version"_s), 2);
    runner().setExecutable(dir.filePath(u"bin/gone"_s));
    EXPECT_EQ(wait(runner().search(u"Kestrel Moon"_s))->error().code, ErrorCode::ToolNotFound);
}

// --- fetch ----------------------------------------------------------------------------------------------------------

TEST_F(YtDlpRunnerTest, FetchWritesTheAudioAndReportsProgress)
{
    YtDlpFetch *job = runner().fetch(u"KmTopic00Ex"_s, cache());
    const auto events = watch(job);
    ASSERT_TRUE(spinAdvancing([&] { return events->finished.has_value(); }));
    ASSERT_TRUE(events->finished->ok()) << events->finished->error().toString().toStdString();
    const FetchedAudio &audio = events->finished->value();
    const QString expected = QDir(cache()).filePath(u"KmTopic00Ex.m4a"_s);
    EXPECT_EQ(audio.videoId, u"KmTopic00Ex"_s);
    EXPECT_EQ(audio.path, expected);
    EXPECT_EQ(audio.format, u"m4a"_s);
    EXPECT_EQ(audio.codec, u"mp4a.40.2"_s);
    EXPECT_EQ(audio.bytes, 40960);
    EXPECT_EQ(QFileInfo(expected).size(), 40960);
    EXPECT_EQ(events->started, QStringList{expected});
    EXPECT_EQ(job->filePath(), expected);
    ASSERT_FALSE(events->progress.isEmpty());
    EXPECT_EQ(events->progress.last(), (std::pair<qint64, qint64>{40960, 40960}));
    const QStringList args = arguments();
    EXPECT_EQ(args.last(), u"https://www.youtube.com/watch?v=KmTopic00Ex"_s);
    EXPECT_EQ(args.at(args.indexOf(u"-P"_s) + 1), QFileInfo(cache()).absoluteFilePath());
    EXPECT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.records().last().outcome, u"ok"_s);
    EXPECT_TRUE(job->isFinished());
    delete job;
}

TEST_F(YtDlpRunnerTest, FetchFailuresLeaveNoPartialFile)
{
    const std::vector<std::pair<QString, ErrorCode>> table{
        {u"die"_s, ErrorCode::ToolTooOld},       // HTTP 403 halfway
        {u"nofile"_s, ErrorCode::ToolFailed},    // exit 0 without a file
        {u"elsewhere"_s, ErrorCode::ToolFailed}, // a file outside the cache folder
        {u"noruntime"_s, ErrorCode::ToolMissingRuntime},
        {u"unavailable"_s, ErrorCode::Unavailable},
        {u"private"_s, ErrorCode::Unavailable},
        {u"drm"_s, ErrorCode::Unavailable},
    };
    for (const auto &[mode, code] : table) {
        setMode(mode);
        const std::unique_ptr<YtDlpFetch> job(runner().fetch(u"KmTopic00Ex"_s, cache()));
        const auto events = watch(job.get());
        ASSERT_TRUE(spinAdvancing([&] { return events->finished.has_value(); })) << mode.toStdString();
        EXPECT_EQ(events->finished->error().code, code)
            << mode.toStdString() << " " << events->finished->error().message.toStdString();
        EXPECT_FALSE(QFileInfo::exists(QDir(cache()).filePath(u"KmTopic00Ex.m4a"_s))) << mode.toStdString();
    }
}

TEST_F(YtDlpRunnerTest, AnOldYtDlpThatFailsTheChallengeWithDeno)
{
    addDeno();
    setMode(u"challenge"_s);
    const std::unique_ptr<YtDlpFetch> job(runner().fetch(u"KmTopic00Ex"_s, cache()));
    const auto events = watch(job.get());
    ASSERT_TRUE(spinAdvancing([&] { return events->finished.has_value(); }));
    EXPECT_EQ(events->finished->error().code, ErrorCode::ToolTooOld);
}

TEST_F(YtDlpRunnerTest, StalledAndEndlessFetchesTimeOut)
{
    config.idleTimeout = 400ms;
    setMode(u"stall"_s);
    const std::unique_ptr<YtDlpFetch> stalled(runner().fetch(u"KmTopic00Ex"_s, cache()));
    const auto stalledEvents = watch(stalled.get());
    ASSERT_TRUE(spinAdvancing([&] { return stalledEvents->finished.has_value(); }));
    EXPECT_EQ(stalledEvents->finished->error().code, ErrorCode::Timeout);
    EXPECT_TRUE(childGone());
    EXPECT_FALSE(QFileInfo::exists(QDir(cache()).filePath(u"KmTopic00Ex.m4a"_s)));

    m_runner.reset();
    config.idleTimeout = 3s;
    config.totalTimeout = 700ms;
    setMode(u"endless"_s);
    const std::unique_ptr<YtDlpFetch> endless(runner().fetch(u"KmTopic00Ex"_s, cache()));
    const auto endlessEvents = watch(endless.get());
    ASSERT_TRUE(spinAdvancing([&] { return endlessEvents->finished.has_value(); }));
    EXPECT_EQ(endlessEvents->finished->error().code, ErrorCode::Timeout);
    EXPECT_FALSE(QFileInfo::exists(QDir(cache()).filePath(u"KmTopic00Ex.m4a"_s)));
}

TEST_F(YtDlpRunnerTest, CancellingAFetchKillsItAndDeletesThePartialFile)
{
    setMode(u"stall"_s);
    YtDlpFetch *job = runner().fetch(u"KmTopic00Ex"_s, cache());
    const auto events = watch(job);
    ASSERT_TRUE(spinAdvancing([&] { return !events->started.isEmpty(); }));
    EXPECT_TRUE(QFileInfo::exists(events->started.first()));
    job->cancel();
    ASSERT_TRUE(spinUntil([&] { return events->finished.has_value(); }));
    EXPECT_EQ(events->finished->error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(childGone());
    EXPECT_FALSE(QFileInfo::exists(events->started.first()));
    delete job;

    // Deleting a job cancels it too, and the next one runs.
    QFile::remove(dir.filePath(u"child.pid"_s));
    const QPointer<YtDlpFetch> doomed = runner().fetch(u"KmTopic00Re"_s, cache());
    const auto doomedEvents = watch(doomed.data());
    ASSERT_TRUE(spinAdvancing([&] { return !doomedEvents->started.isEmpty(); }));
    delete doomed.data();
    EXPECT_TRUE(childGone());
    EXPECT_FALSE(QFileInfo::exists(QDir(cache()).filePath(u"KmTopic00Re.m4a"_s)));
    setMode(u"ok"_s);
    EXPECT_TRUE(wait(runner().search(u"Kestrel Moon"_s))->ok());
}

TEST_F(YtDlpRunnerTest, ACancelledFetchCleansUpEvenBeforeTheFormatIsNamed)
{
    // An older preview of the same video in another container stays; what this run wrote goes.
    const QString older = QDir(cache()).filePath(u"KmTopic00Ex.webm"_s);
    writeFile(older, "older preview");
    QFile oldFile(older);
    ASSERT_TRUE(oldFile.open(QIODevice::ReadWrite));
    ASSERT_TRUE(oldFile.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2), QFileDevice::FileModificationTime));
    oldFile.close();
    setMode(u"unnamed"_s);
    const std::unique_ptr<YtDlpFetch> job(runner().fetch(u"KmTopic00Ex"_s, cache()));
    const auto events = watch(job.get());
    const QString partial = QDir(cache()).filePath(u"KmTopic00Ex.m4a"_s);
    ASSERT_TRUE(spinAdvancing([&] { return QFileInfo::exists(dir.filePath(u"child.pid"_s)); }));
    ASSERT_TRUE(QFileInfo::exists(partial));
    job->cancel();
    ASSERT_TRUE(spinUntil([&] { return events->finished.has_value(); }));
    EXPECT_TRUE(childGone());
    EXPECT_TRUE(spinUntil([&] { return !QFileInfo::exists(partial); }));
    EXPECT_TRUE(QFileInfo::exists(older));
    EXPECT_TRUE(events->started.isEmpty());
}

TEST_F(YtDlpRunnerTest, DestroyingTheRunnerKillsTheProcess)
{
    setMode(u"stall"_s);
    const std::unique_ptr<YtDlpFetch> job(runner().fetch(u"KmTopic00Ex"_s, cache()));
    const auto events = watch(job.get());
    ASSERT_TRUE(spinAdvancing([&] { return !events->started.isEmpty(); }));
    m_runner.reset();
    EXPECT_TRUE(childGone());
    EXPECT_FALSE(QFileInfo::exists(events->started.first()));
    ASSERT_TRUE(spinUntil([&] { return events->finished.has_value(); }));
    EXPECT_EQ(events->finished->error().code, ErrorCode::Cancelled);
    job->cancel(); // harmless once finished
}

TEST_F(YtDlpRunnerTest, DestroyingTheRunnerMidSearchOrMidVersionCheckLeavesNoChild)
{
    setMode(u"hang"_s);
    const QFuture<Result<QList<YouTubeCandidate>>> search = runner().search(u"Kestrel Moon"_s);
    ASSERT_TRUE(spinAdvancing([&] { return QFileInfo::exists(dir.filePath(u"child.pid"_s)); }));
    m_runner.reset();
    EXPECT_TRUE(childGone());
    ASSERT_TRUE(spinUntil([&] { return search.isFinished(); }));
    EXPECT_TRUE(search.isCanceled());

    QFile::remove(dir.filePath(u"child.pid"_s));
    setMode(u"versionhang"_s);
    const QFuture<Result<YtDlpInstall>> probe = runner().probe();
    ASSERT_TRUE(spinUntil([&] { return QFileInfo::exists(dir.filePath(u"child.pid"_s)); }));
    m_runner.reset();
    EXPECT_TRUE(childGone());
    EXPECT_TRUE(probe.isCanceled());
}

TEST_F(YtDlpRunnerTest, FetchRejectsBadInputAtOnce)
{
    for (const auto &[id, folder] : std::vector<std::pair<QString, QString>>{
             {u"short"_s, cache()},
             {u"KmTopic00Ex"_s, dir.filePath(u"missing"_s)},
             {u"KmTopic00Ex"_s, u"cache"_s},
             {u"KmTopic00Ex"_s, QString()},
         }) {
        const std::unique_ptr<YtDlpFetch> job(runner().fetch(id, folder));
        EXPECT_TRUE(job->isFinished());
        const auto events = watch(job.get());
        ASSERT_TRUE(spinUntil([&] { return events->finished.has_value(); }));
        EXPECT_EQ(events->finished->error().code, ErrorCode::InvalidArgument) << id.toStdString();
    }
    EXPECT_TRUE(runs().isEmpty());
}
