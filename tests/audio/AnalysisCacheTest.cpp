// SPDX-License-Identifier: GPL-3.0-or-later
// The analysis cache (A2): blob round trips, corruption and version handling, two-tier LRU eviction, content keys.
#include "audio/AnalysisCache.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/WaveformAnalyzer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::audio;
using namespace selecta::testing;

namespace {

Waveform syntheticWaveform(qint64 seconds, int seed)
{
    const int rate = 44100;
    const qint64 frames = seconds * rate;
    QList<WaveformColumn> detail(columnCountForFrames(frames, rate));
    for (qsizetype i = 0; i < detail.size(); ++i) {
        for (std::size_t b = 0; b < kBandCount; ++b) {
            detail[i].peak.at(b) = static_cast<quint8>(((i * (b + 3)) + seed) % 256);
            detail[i].rms.at(b) = static_cast<quint8>(((i * (b + 1)) + seed) % 200);
        }
    }
    return Waveform::fromDetail(rate, frames, detail, true);
}

CacheKey keyFor(int n)
{
    return CacheKey::forName(u"peer|track %1|1000"_s.arg(n));
}

} // namespace

TEST(AnalysisCacheBlobs, RoundTripDetailAndOverview)
{
    const Waveform waveform = syntheticWaveform(200, 1);
    const QByteArray detail = AnalysisCache::encodeDetail(waveform);
    const Result<Waveform> decoded = AnalysisCache::decodeDetail(detail);
    ASSERT_TRUE(decoded.ok()) << decoded.error().toString().toStdString();
    EXPECT_EQ(decoded.value(), waveform);
    EXPECT_LT(detail.size(), waveform.detail().size() * 6); // compressed

    const QByteArray overview = AnalysisCache::encodeOverview(waveform);
    EXPECT_EQ(overview.size(), 36 + (1200 * 3) + 4); // ~3.6 KB
    const Result<Waveform> brief = AnalysisCache::decodeOverview(overview);
    ASSERT_TRUE(brief.ok());
    EXPECT_FALSE(brief.value().hasDetail());
    EXPECT_EQ(brief.value().overview(), waveform.overview());
    EXPECT_EQ(brief.value().frameCount(), waveform.frameCount());
}

TEST(AnalysisCacheBlobs, CorruptionIsAlwaysDetected)
{
    const Waveform waveform = syntheticWaveform(60, 2);
    const QByteArray blob = AnalysisCache::encodeDetail(waveform);
    // Every single-byte change anywhere is caught (header fields, payload, checksum).
    for (qsizetype i = 0; i < blob.size(); i += std::max<qsizetype>(1, blob.size() / 97)) {
        QByteArray damaged = blob;
        damaged[i] = static_cast<char>(static_cast<quint8>(damaged[i]) ^ 0x40U);
        EXPECT_FALSE(AnalysisCache::decodeDetail(damaged).ok()) << "byte " << i;
    }
    for (const qsizetype length : {qsizetype{0}, qsizetype{10}, qsizetype{39}, blob.size() - 1}) {
        EXPECT_FALSE(AnalysisCache::decodeDetail(blob.left(length)).ok()) << length;
    }
    EXPECT_FALSE(AnalysisCache::decodeOverview(blob).ok()); // wrong kind
    EXPECT_FALSE(AnalysisCache::decodeDetail(randomBytes(5000, 4)).ok());
}

TEST(AnalysisCacheBlobs, HostileSizesDoNotAllocate)
{
    // A valid-looking header announcing a huge column count with a tiny payload: refused before inflating.
    const Waveform waveform = syntheticWaveform(1, 3);
    QByteArray blob = AnalysisCache::encodeDetail(waveform);
    blob[28] = static_cast<char>(0xFF);
    blob[29] = static_cast<char>(0xFF);
    blob[30] = static_cast<char>(0xFF);
    blob[31] = static_cast<char>(0x7F);
    EXPECT_FALSE(AnalysisCache::decodeDetail(blob).ok());
}

TEST(AnalysisCache, StoreLoadAndOverviewOnlyAfterEviction)
{
    const QTemporaryDir dir;
    const Waveform waveform = syntheticWaveform(120, 5);
    const qint64 detailSize = AnalysisCache::encodeDetail(waveform).size();
    // Room for two details.
    AnalysisCache cache(dir.path(), (detailSize * 2) + 100);
    EXPECT_FALSE(cache.load(keyFor(1)));
    ASSERT_TRUE(cache.store(keyFor(1), waveform).ok());
    std::optional<Waveform> hit = cache.load(keyFor(1));
    ASSERT_TRUE(hit);
    EXPECT_TRUE(hit->hasDetail());
    EXPECT_EQ(*hit, waveform);

    ASSERT_TRUE(cache.store(keyFor(2), syntheticWaveform(120, 6)).ok());
    ASSERT_TRUE(cache.load(keyFor(1))); // 1 is now the most recent
    ASSERT_TRUE(cache.store(keyFor(3), syntheticWaveform(120, 7)).ok());
    EXPECT_LE(cache.detailBytes(), (detailSize * 2) + 100);
    // 2 was least recently used: its detail is gone, its overview stays.
    const std::optional<Waveform> evicted = cache.load(keyFor(2));
    ASSERT_TRUE(evicted);
    EXPECT_FALSE(evicted->hasDetail());
    EXPECT_EQ(evicted->overview().size(), kOverviewColumns);
    EXPECT_TRUE(cache.load(keyFor(1))->hasDetail());
    EXPECT_TRUE(cache.load(keyFor(3))->hasDetail());
}

TEST(AnalysisCache, OverviewTierIsBoundedToo)
{
    const QTemporaryDir dir;
    const qint64 overviewSize = AnalysisCache::encodeOverview(syntheticWaveform(10, 1)).size();
    AnalysisCache cache(dir.path(), 0, overviewSize * 3);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(cache.store(keyFor(i), syntheticWaveform(10, i)).ok());
    }
    EXPECT_LE(cache.overviewBytes(), overviewSize * 3);
    EXPECT_FALSE(cache.load(keyFor(0)));
    EXPECT_TRUE(cache.load(keyFor(9)));
    EXPECT_EQ(cache.detailBytes(), 0); // a zero detail budget keeps no detail
}

TEST(AnalysisCache, CorruptFilesAreDiscardedAndRecencySurvivesRestarts)
{
    const QTemporaryDir dir;
    {
        AnalysisCache cache(dir.path());
        ASSERT_TRUE(cache.store(keyFor(1), syntheticWaveform(30, 1)).ok());
        ASSERT_TRUE(cache.store(keyFor(2), syntheticWaveform(30, 2)).ok());
    }
    // Damage the detail of 1 on disk.
    const QString detailPath = dir.filePath(u"detail/"_s + keyFor(1).toHex() + u".swd"_s);
    QFile file(detailPath);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    file.seek(50);
    file.write("garbage");
    file.close();

    AnalysisCache cache(dir.path());
    const std::optional<Waveform> damaged = cache.load(keyFor(1));
    ASSERT_TRUE(damaged); // falls back to the intact overview
    EXPECT_FALSE(damaged->hasDetail());
    EXPECT_FALSE(QFile::exists(detailPath)); // deleted so it is recomputed
    EXPECT_TRUE(cache.load(keyFor(2))->hasDetail());

    cache.remove(keyFor(2));
    EXPECT_FALSE(cache.load(keyFor(2)));
    cache.remove(keyFor(99)); // missing: no error
}

TEST(AnalysisCache, RefusesPartialWaveformsAndNullKeys)
{
    const QTemporaryDir dir;
    AnalysisCache cache(dir.path());
    const Waveform partial = Waveform::fromDetail(44100, 44100, QList<WaveformColumn>(150), false);
    EXPECT_EQ(cache.store(keyFor(1), partial).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(cache.store(CacheKey{}, syntheticWaveform(5, 1)).error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(cache.load(CacheKey{}));
}

TEST(AnalysisCache, UnwritableDirectoryFailsCleanly)
{
    const QTemporaryDir dir;
    const QString blocker = dir.filePath(u"file"_s);
    QFile file(blocker);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    AnalysisCache cache(blocker + u"/cache"_s); // a path under a regular file can't be created
    EXPECT_EQ(cache.store(keyFor(1), syntheticWaveform(5, 1)).error().code, ErrorCode::WriteFailed);
}

TEST(CacheKeys, ContentKeysFollowTheBytes)
{
    const QByteArray a = makeWav(sine(440, 10.0));
    QByteArray b = a;
    b[b.size() / 2] = static_cast<char>(static_cast<quint8>(b[b.size() / 2]) ^ 1U);
    MemorySource sourceA(a);
    MemorySource sourceA2(a);
    MemorySource sourceTail(a.left(a.size() - 1));
    const CacheKey ka = CacheKey::forContent(sourceA).value();
    EXPECT_FALSE(ka.isNull());
    EXPECT_EQ(ka, CacheKey::forContent(sourceA2).value());
    EXPECT_NE(ka, CacheKey::forContent(sourceTail).value());
    // A change in the middle is caught only when it falls in a sampled block; changes at either end always are.
    QByteArray head = a;
    head[100] = static_cast<char>(static_cast<quint8>(head[100]) ^ 1U);
    MemorySource sourceHead(head);
    EXPECT_NE(ka, CacheKey::forContent(sourceHead).value());
    MemorySource small(QByteArray("tiny"));
    EXPECT_TRUE(CacheKey::forContent(small).ok());
    MemorySource growing(a, 100);
    EXPECT_EQ(CacheKey::forContent(growing).error().code, ErrorCode::InvalidArgument);
}

TEST(CacheKeys, HexAndNames)
{
    const CacheKey key = CacheKey::forName(u"peer|path|123"_s);
    EXPECT_EQ(CacheKey::fromHex(key.toHex()), key);
    EXPECT_NE(CacheKey::forName(u"peer|path|124"_s), key);
    EXPECT_FALSE(CacheKey::fromHex(u"zz"_s));
    EXPECT_FALSE(CacheKey::fromHex(QString(64, u'g')));
    EXPECT_TRUE(CacheKey().isNull());
    EXPECT_EQ(qHash(key), qHash(*CacheKey::fromHex(key.toHex())));
}

TEST(AnalysisCache, RealAnalysisRoundTrip)
{
    const QTemporaryDir dir;
    const auto source = std::make_shared<MemorySource>(makeFlac(quantised(tones({60, 1000, 8000}, 20.0), 16)));
    const Result<Waveform> waveform = analyzeWaveform(source);
    ASSERT_TRUE(waveform.ok());
    AnalysisCache cache(dir.path());
    const CacheKey key = CacheKey::forContent(*source).value();
    ASSERT_TRUE(cache.store(key, waveform.value()).ok());
    EXPECT_EQ(cache.load(key), waveform.value());
}
