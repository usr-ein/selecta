// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/YouTubePreviews.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/GovernorPacer.h"
#include "governor/Governor.h"
#include "meta/YtDlpRunner.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::Preview;
using selecta::core::YouTubePreviews;

// The fake yt-dlp from meta's tests; the real one never runs in tests (CLAUDE.md).
namespace {

QString fixtures()
{
    return QStringLiteral(SELECTA_META_FIXTURES);
}

bool writeExecutable(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
        return false;
    }
    file.close();
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

selecta::meta::Release glassHarbour()
{
    selecta::meta::Release release;
    release.title = u"Glass Harbour (Extended Mix)"_s;
    release.artists = {u"Kestrel Moon"_s};
    release.durationMs = 391'000;
    return release;
}

class YouTubePreviewsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        const QString wrapper =
            u"#!/bin/sh\n"
            u"FAKE_YTDLP_MODE=\"$(cat '%1/mode' 2>/dev/null || echo ok)\"\n"
            u"FAKE_YTDLP_SEARCH=\"$(cat '%1/search' 2>/dev/null || echo '%2')\"\n"
            u"FAKE_YTDLP_TOPIC_SEARCH=\"$(cat '%1/topic-search' 2>/dev/null)\"\n"
            u"FAKE_YTDLP_LOG='%1/runs'\n"
            u"export FAKE_YTDLP_MODE FAKE_YTDLP_SEARCH FAKE_YTDLP_TOPIC_SEARCH FAKE_YTDLP_LOG\n"
            u"exec /bin/sh '%3' \"$@\"\n"_s.arg(dir.path(), fixtures() + u"/youtube/search_glass_harbour.json"_s,
                                                fixtures() + u"/fake-yt-dlp"_s);
        ASSERT_TRUE(writeExecutable(dir.filePath(u"bin/yt-dlp"_s), wrapper.toUtf8()));
        ASSERT_TRUE(writeExecutable(dir.filePath(u"bin/deno"_s), "#!/bin/sh\nexit 0\n"));
        config.executable = dir.filePath(u"bin/yt-dlp"_s);
        config.searchDirectories = {dir.filePath(u"bin"_s)};
        config.today = [] { return QDate(2026, 10, 20); };
    }

    void setMode(const QString &mode) { write(u"mode"_s, mode); }
    // The search results the fake prints: for any search, and for a search of the artist's Topic channel.
    void setSearches(const QString &search, const QString &topicSearch)
    {
        write(u"search"_s, fixtures() + u"/youtube/"_s + search);
        write(u"topic-search"_s, fixtures() + u"/youtube/"_s + topicSearch);
    }

    void write(const QString &name, const QString &content)
    {
        QFile file(dir.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(content.toUtf8());
    }

    int searches() const
    {
        QFile file(dir.filePath(u"runs"_s));
        return file.open(QIODevice::ReadOnly) ? static_cast<int>(file.readAll().count("start search")) : 0;
    }

    // Runs the event loop, moving the clock on (yt-dlp runs are paced ≥ 2 s apart), until `done`.
    bool eventually(const std::function<bool()> &done)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > 30'000) {
                return false;
            }
            QTest::qWait(20);
            clock.advance(500ms);
        }
        return true;
    }

    QTemporaryDir dir;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    QObject context; // the pacer delivers its decisions on this object's thread
    selecta::core::GovernorPacer pacer{governor, log, &context};
    selecta::meta::YtDlpRunner::Config config;
};

} // namespace

TEST_F(YouTubePreviewsTest, FindsTheTrackAndPlaysItWhileItArrives)
{
    selecta::meta::YtDlpRunner runner(config, pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy started(&previews, &YouTubePreviews::started);
    QSignalSpy changes(&previews, &YouTubePreviews::changed);
    const qint64 id = previews.start(u"line:7"_s, glassHarbour());
    ASSERT_TRUE(eventually([&] { return !started.isEmpty(); }));
    EXPECT_EQ(started.at(0).at(0).toString(), u"line:7"_s);
    EXPECT_EQ(started.at(0).at(1).toLongLong(), id);
    const auto source = started.at(0).at(2).value<std::shared_ptr<selecta::audio::ByteSource>>();
    ASSERT_TRUE(source);
    EXPECT_EQ(source->totalSize(), 40'960);
    EXPECT_EQ(started.at(0).at(3).toString(), u"youtube|KmTopic00Ex"_s); // the official Topic channel's upload
    EXPECT_TRUE(started.at(0).at(6).toString().contains(u"YouTube"_s));
    EXPECT_EQ(searches(), 1); // official already: no second search
    ASSERT_TRUE(eventually([&] {
        return !changes.isEmpty() && changes.constLast().at(0).value<Preview>().state == Preview::State::Complete;
    }));
    EXPECT_TRUE(source->snapshot().isComplete());
}

TEST_F(YouTubePreviewsTest, FanUploadsOnlyLeadToASearchOfTheArtistsTopicChannel)
{
    // Seen live (2026-10-20): the first page held fan uploads only, and one of them was played.
    setSearches(u"search_fan_uploads.json"_s, u"search_topic_glass_harbour.json"_s);
    selecta::meta::YtDlpRunner runner(config, pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy started(&previews, &YouTubePreviews::started);
    previews.start(u"line:7"_s, glassHarbour());
    ASSERT_TRUE(eventually([&] { return !started.isEmpty(); }));
    EXPECT_EQ(started.at(0).at(3).toString(), u"youtube|KmTopic00Ex"_s);
    EXPECT_TRUE(started.at(0).at(6).toString().contains(u"Kestrel Moon - Topic"_s));
    EXPECT_EQ(searches(), 2);
}

TEST_F(YouTubePreviewsTest, WhenTheTopicSearchFindsNothingOfficialTheFirstPickStands)
{
    setSearches(u"search_fan_uploads.json"_s, u"search_fan_uploads.json"_s);
    selecta::meta::YtDlpRunner runner(config, pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy started(&previews, &YouTubePreviews::started);
    previews.start(u"line:7"_s, glassHarbour());
    ASSERT_TRUE(eventually([&] { return !started.isEmpty(); }));
    EXPECT_EQ(started.at(0).at(3).toString(), u"youtube|FanUpload01"_s); // within ±3 s, most viewed
    EXPECT_EQ(searches(), 2);
}

TEST_F(YouTubePreviewsTest, SaysWhyWhenItCantBeHeard)
{
    setMode(u"unavailable"_s);
    selecta::meta::YtDlpRunner runner(config, pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy changes(&previews, &YouTubePreviews::changed);
    QString unavailable;
    previews.start(u"line:7"_s, glassHarbour(), [&unavailable](const QString &why) { unavailable = why; });
    ASSERT_TRUE(eventually([&] {
        return !changes.isEmpty() && changes.constLast().at(0).value<Preview>().state == Preview::State::Failed;
    }));
    EXPECT_FALSE(changes.constLast().at(0).value<Preview>().message.isEmpty());
    EXPECT_EQ(unavailable, changes.constLast().at(0).value<Preview>().message); // the caller can try elsewhere
}

TEST_F(YouTubePreviewsTest, WithoutALengthNoVideoIsGuessed)
{
    selecta::meta::YtDlpRunner runner(config, pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy changes(&previews, &YouTubePreviews::changed);
    selecta::meta::Release release = glassHarbour();
    release.durationMs.reset();
    previews.start(u"line:7"_s, release);
    ASSERT_TRUE(eventually([&] {
        return !changes.isEmpty() && changes.constLast().at(0).value<Preview>().state == Preview::State::Failed;
    }));
    EXPECT_TRUE(changes.constLast().at(0).value<Preview>().message.contains(u"length"_s));
}

TEST_F(YouTubePreviewsTest, IsolatedRunsNeverRunYtDlp)
{
    selecta::meta::YtDlpRunner runner(selecta::meta::YtDlpRunner::Config::disabled(), pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy changes(&previews, &YouTubePreviews::changed);
    previews.start(u"line:7"_s, glassHarbour());
    ASSERT_TRUE(eventually([&] {
        return !changes.isEmpty() && changes.constLast().at(0).value<Preview>().state == Preview::State::Failed;
    }));
    EXPECT_FALSE(QFile::exists(dir.filePath(u"previews"_s) + u"/KmTopic00Ex.m4a"_s));
}

TEST_F(YouTubePreviewsTest, StoppingCancelsTheFetch)
{
    setMode(u"stall"_s);
    selecta::meta::YtDlpRunner runner(config, pacer, clock);
    YouTubePreviews previews(runner, dir.filePath(u"previews"_s));
    QSignalSpy changes(&previews, &YouTubePreviews::changed);
    previews.start(u"line:7"_s, glassHarbour());
    ASSERT_TRUE(eventually([&] {
        return !changes.isEmpty() && changes.constLast().at(0).value<Preview>().message.contains(u"Fetching"_s);
    }));
    previews.stop();
    // The partial file goes with the cancelled fetch.
    ASSERT_TRUE(eventually([&] { return !QFile::exists(dir.filePath(u"previews/KmTopic00Ex.m4a"_s)); }));
}
