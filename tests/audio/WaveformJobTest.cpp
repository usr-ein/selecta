// SPDX-License-Identifier: GPL-3.0-or-later
// WaveformJob (A2): asynchronous analysis of complete and downloading sources, cache hits, cancellation, failures.
#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/WaveformAnalyzer.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::audio;
using namespace selecta::testing;

namespace {

constexpr int kTimeoutMs = 60000;

bool waitForEnd(const QSignalSpy &finished, const QSignalSpy &failed)
{
    return QTest::qWaitFor([&] { return finished.count() + failed.count() > 0; }, kTimeoutMs);
}

} // namespace

TEST(WaveformJob, CompleteSourceReportsFinished)
{
    const auto source = std::make_shared<MemorySource>(makeFlac(quantised(tones({60, 1000}, 25.0), 16)));
    WaveformJob job(source);
    const QSignalSpy finished(&job, &WaveformJob::finished);
    const QSignalSpy failed(&job, &WaveformJob::failed);
    const QSignalSpy columns(&job, &WaveformJob::columnsAdded);
    job.start();
    job.start(); // once only
    EXPECT_TRUE(job.isRunning());
    ASSERT_TRUE(waitForEnd(finished, failed));
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(columns.count(), 0); // complete sources report finished() directly
    EXPECT_FALSE(job.isRunning());
    const auto waveform = finished.first().first().value<Waveform>();
    EXPECT_EQ(waveform, analyzeWaveform(source).value());
}

TEST(WaveformJob, DownloadingSourceStreamsColumnsThenFinishes)
{
    const QByteArray file = makeMp3(quantised(tones({60, 1000, 8000}, 12.0), 16), {});
    const auto growing = std::make_shared<LoggingSource>(file, 0);
    WaveformJob job(growing);
    const QSignalSpy finished(&job, &WaveformJob::finished);
    const QSignalSpy failed(&job, &WaveformJob::failed);
    QList<WaveformColumn> streamed;
    qint64 expected = -2;
    QObject::connect(&job, &WaveformJob::columnsAdded, &job,
                     [&](const QList<WaveformColumn> &columns, qint64 firstColumn, qint64 expectedColumns) {
                         EXPECT_EQ(firstColumn, streamed.size());
                         streamed.append(columns);
                         expected = expectedColumns;
                     });
    job.start();
    // The "download": 16 KB every 2 ms on this thread's event loop.
    QTimer download;
    qint64 available = 0;
    QObject::connect(&download, &QTimer::timeout, &download, [&] {
        available = std::min<qint64>(file.size(), available + 16384);
        growing->setAvailable(available);
        if (available == file.size()) {
            growing->setComplete();
            download.stop();
        }
    });
    download.start(2);
    ASSERT_TRUE(waitForEnd(finished, failed));
    ASSERT_EQ(finished.count(), 1) << (failed.isEmpty()
                                           ? ""
                                           : failed.first().first().value<Error>().toString().toStdString());
    const auto waveform = finished.first().first().value<Waveform>();
    EXPECT_TRUE(waveform.isComplete());
    EXPECT_EQ(streamed, waveform.detail());
    EXPECT_EQ(expected, waveform.detail().size());
    EXPECT_EQ(waveform, analyzeWaveform(std::make_shared<MemorySource>(file)).value());
    EXPECT_EQ(growing->violations(), 0);
}

TEST(WaveformJob, Mp4WithIndexAtTheEndWaitsForTheDownload)
{
    const QByteArray file = makeM4a(sine(440, 3.0), {});
    const auto growing = std::make_shared<MemorySource>(file, file.size() / 2);
    WaveformJob job(growing);
    const QSignalSpy waiting(&job, &WaveformJob::waitingForCompleteFile);
    const QSignalSpy finished(&job, &WaveformJob::finished);
    const QSignalSpy failed(&job, &WaveformJob::failed);
    job.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return waiting.count() == 1; }, kTimeoutMs));
    growing->setAvailable(file.size() - 100);
    QTest::qWait(50);
    EXPECT_EQ(waiting.count(), 1); // announced once
    EXPECT_EQ(finished.count(), 0);
    growing->setComplete();
    ASSERT_TRUE(waitForEnd(finished, failed));
    EXPECT_EQ(finished.count(), 1);
}

TEST(WaveformJob, CacheHitSkipsDecoding)
{
    const QTemporaryDir dir;
    const auto cache = std::make_shared<AnalysisCache>(dir.path());
    const auto real = std::make_shared<MemorySource>(makeWav(sine(440, 5.0)));
    const Waveform waveform = analyzeWaveform(real).value();
    const CacheKey key = CacheKey::forName(u"peer|song.wav|1234"_s);
    ASSERT_TRUE(cache->store(key, waveform).ok());
    // The job's source is garbage: only a cache hit can produce a waveform.
    for (const qint64 available : {qint64{-1}, qint64{10}}) {
        WaveformJob job(std::make_shared<MemorySource>(randomBytes(1000, 1), available));
        job.setCache(cache, key);
        const QSignalSpy finished(&job, &WaveformJob::finished);
        const QSignalSpy failed(&job, &WaveformJob::failed);
        job.start();
        ASSERT_TRUE(waitForEnd(finished, failed));
        ASSERT_EQ(finished.count(), 1);
        EXPECT_EQ(finished.first().first().value<Waveform>(), waveform);
    }
}

TEST(WaveformJob, AnalysisIsStoredInTheCache)
{
    const QTemporaryDir dir;
    const auto cache = std::make_shared<AnalysisCache>(dir.path());
    const auto source = std::make_shared<MemorySource>(makeWav(sine(440, 5.0)));
    const CacheKey key = CacheKey::forContent(*source).value();
    WaveformJob job(source);
    job.setCache(cache, key);
    const QSignalSpy finished(&job, &WaveformJob::finished);
    const QSignalSpy failed(&job, &WaveformJob::failed);
    job.start();
    ASSERT_TRUE(waitForEnd(finished, failed));
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(cache->load(key), finished.first().first().value<Waveform>());
}

TEST(WaveformJob, FailuresAreReported)
{
    WaveformJob job(std::make_shared<MemorySource>(randomBytes(300000, 2)));
    const QSignalSpy finished(&job, &WaveformJob::finished);
    const QSignalSpy failed(&job, &WaveformJob::failed);
    job.start();
    ASSERT_TRUE(waitForEnd(finished, failed));
    ASSERT_EQ(failed.count(), 1);
    EXPECT_EQ(failed.first().first().value<Error>().code, ErrorCode::Unsupported);

    const QByteArray mp3 = makeMp3(sine(440, 5.0), {});
    const auto growing = std::make_shared<MemorySource>(mp3, mp3.size() / 3);
    WaveformJob preview(growing);
    const QSignalSpy previewFailed(&preview, &WaveformJob::failed);
    preview.start();
    QTest::qWait(100);
    growing->setFailed(); // the download died
    ASSERT_TRUE(QTest::qWaitFor([&] { return previewFailed.count() == 1; }, kTimeoutMs));
    EXPECT_EQ(previewFailed.first().first().value<Error>().code, ErrorCode::SourceFailed);
}

TEST(WaveformJob, CancelSilencesTheJob)
{
    const auto source = std::make_shared<MemorySource>(makeWav(whiteNoise(60.0, 9)));
    WaveformJob job(source);
    const QSignalSpy finished(&job, &WaveformJob::finished);
    const QSignalSpy failed(&job, &WaveformJob::failed);
    job.start();
    job.cancel();
    EXPECT_FALSE(job.isRunning());
    QThreadPool::globalInstance()->waitForDone();
    QTest::qWait(50);
    EXPECT_EQ(finished.count() + failed.count(), 0);
}

TEST(WaveformJob, DestroyingARunningJobIsSafe)
{
    const QByteArray file = makeFlac(quantised(whiteNoise(30.0, 4), 16));
    for (int i = 0; i < 5; ++i) {
        auto *job = new WaveformJob(std::make_shared<MemorySource>(file, file.size() / 2));
        job->start();
        auto *complete = new WaveformJob(std::make_shared<MemorySource>(file));
        complete->start();
        delete job;
        delete complete;
    }
    QThreadPool::globalInstance()->waitForDone();
    QTest::qWait(20); // any stray queued call would land on deleted objects here
    SUCCEED();
}
