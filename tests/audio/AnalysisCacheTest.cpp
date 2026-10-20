// SPDX-License-Identifier: GPL-3.0-or-later
// The analysis cache (A2, A7): blob round trips, corruption and version handling, LRU eviction in each tier (overview,
// detail, quality reports), content keys.
#include "audio/AnalysisCache.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/QualityCheck.h"
#include "audio/WaveformAnalyzer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <zlib.h>

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

// A report with every field set (findings out of enum order) and, unless columns is negative, a spectrogram of
// columns × bins patterned levels (smooth enough to compress, like a real one).
QualityReport syntheticReport(int columns = 800, int bins = 256, int seed = 1)
{
    QualityReport report;
    report.cutoffHz = 16742;
    report.likelySourceKbps = 128;
    report.lameLowpassHz = 17000;
    report.sourceBitDepth = 24;
    report.effectiveBitDepth = 16;
    report.sampleRate = 44100;
    report.contentBandwidthHz = -1;
    report.findings = {QualityFinding::LikelyPadded24Bit, QualityFinding::LikelyFromLossy};
    if (columns >= 0) {
        QByteArray levels(qsizetype{columns} * bins, '\0');
        for (qsizetype i = 0; i < levels.size(); ++i) {
            const qsizetype bin = i % bins;
            levels[i] = static_cast<char>(bin < bins * 3 / 4 ? 150 - (bin / 4) + (((i / bins) + seed) % 7) : 0);
        }
        report.spectrogram = Spectrogram::fromLevels(44100, qint64{44100} * 360, columns, bins, levels).value();
    }
    return report;
}

// Writes a fresh CRC over a deliberately changed blob, so what is tested is the validation behind the checksum.
QByteArray resealed(QByteArray blob)
{
    const qsizetype body = blob.size() - 4;
    const auto crc =
        static_cast<quint32>(crc32(0, reinterpret_cast<const Bytef *>(blob.constData()), static_cast<uInt>(body)));
    for (int i = 0; i < 4; ++i) {
        blob[body + i] = static_cast<char>((crc >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
    return blob;
}

QByteArray withLe32(QByteArray blob, qsizetype offset, quint32 value)
{
    for (int i = 0; i < 4; ++i) {
        blob[offset + i] = static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
    return blob;
}

QByteArray withLe16(QByteArray blob, qsizetype offset, quint32 value)
{
    blob[offset] = static_cast<char>(value & 0xFFU);
    blob[offset + 1] = static_cast<char>((value >> 8U) & 0xFFU);
    return blob;
}

QString qualityPath(const QTemporaryDir &dir, const CacheKey &key)
{
    return dir.filePath(u"quality/"_s + key.toHex() + u".sqr"_s);
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

TEST(AnalysisCache, SaysItsSizeAndEmpties)
{
    const QTemporaryDir dir;
    {
        AnalysisCache cache(dir.path());
        ASSERT_TRUE(cache.store(keyFor(1), syntheticWaveform(60, 1)).ok());
        ASSERT_TRUE(cache.store(keyFor(2), syntheticWaveform(60, 2)).ok());
    }
    // A new process: the size comes from the folder.
    AnalysisCache cache(dir.path());
    const qint64 total = cache.totalBytes();
    EXPECT_GT(total, 0);
    EXPECT_EQ(total, cache.detailBytes() + cache.overviewBytes());
    cache.clear();
    EXPECT_EQ(cache.totalBytes(), 0);
    EXPECT_FALSE(cache.load(keyFor(1)));
    EXPECT_TRUE(QDir(dir.path() + u"/detail"_s).entryList(QDir::Files).isEmpty());
    EXPECT_TRUE(QDir(dir.path() + u"/overview"_s).entryList(QDir::Files).isEmpty());
    // And fills again as before.
    ASSERT_TRUE(cache.store(keyFor(3), syntheticWaveform(60, 3)).ok());
    EXPECT_TRUE(cache.load(keyFor(3)));
}

// ---------------------------------------------------------------------------------------------------------------
// Quality reports (A7)

TEST(AnalysisCacheBlobs, QualityReportsRoundTrip)
{
    const QualityReport full = syntheticReport();
    const QByteArray blob = AnalysisCache::encodeQuality(full);
    ASSERT_FALSE(blob.isEmpty());
    const Result<QualityReport> decoded = AnalysisCache::decodeQuality(blob);
    ASSERT_TRUE(decoded.ok()) << decoded.error().toString().toStdString();
    EXPECT_EQ(decoded.value(), full);
    EXPECT_EQ(decoded.value().findings.first(), QualityFinding::LikelyPadded24Bit); // order kept
    EXPECT_LT(blob.size(), 800 * 256 / 2);                                          // compressed

    // Without a spectrogram: a few dozen bytes.
    const QualityReport brief = syntheticReport(-1);
    const QByteArray small = AnalysisCache::encodeQuality(brief);
    EXPECT_EQ(small.size(), 64);
    EXPECT_EQ(AnalysisCache::decodeQuality(small).value(), brief);

    // The defaults (nothing measured), every finding, an empty spectrogram, the largest grid.
    QualityReport bare;
    bare.sampleRate = 8000;
    EXPECT_EQ(AnalysisCache::decodeQuality(AnalysisCache::encodeQuality(bare)).value(), bare);
    QualityReport all = syntheticReport(4, 2);
    all.findings = {QualityFinding::LikelyUpsampled, QualityFinding::LikelyReencoded, QualityFinding::LikelyFromLossy,
                    QualityFinding::LikelyUpconvertedMp3, QualityFinding::LikelyPadded24Bit};
    all.likelySourceKbps.reset();
    all.contentBandwidthHz = 22050;
    EXPECT_EQ(AnalysisCache::decodeQuality(AnalysisCache::encodeQuality(all)).value(), all);
    QualityReport empty = syntheticReport(0, 256);
    EXPECT_TRUE(empty.spectrogram->isEmpty());
    EXPECT_EQ(AnalysisCache::decodeQuality(AnalysisCache::encodeQuality(empty)).value(), empty);
    const QualityReport largest = syntheticReport(kSpectrogramMaxColumns, kSpectrogramMaxBins);
    EXPECT_EQ(AnalysisCache::decodeQuality(AnalysisCache::encodeQuality(largest)).value(), largest);
}

TEST(AnalysisCacheBlobs, QualityCorruptionIsAlwaysDetected)
{
    const QByteArray blob = AnalysisCache::encodeQuality(syntheticReport(200, 64));
    for (qsizetype i = 0; i < blob.size(); i += std::max<qsizetype>(1, blob.size() / 211)) {
        QByteArray damaged = blob;
        damaged[i] = static_cast<char>(static_cast<quint8>(damaged[i]) ^ 0x21U);
        EXPECT_FALSE(AnalysisCache::decodeQuality(damaged).ok()) << "byte " << i;
    }
    for (qsizetype i = 0; i < 64; ++i) { // every header byte
        QByteArray damaged = blob;
        damaged[i] = static_cast<char>(static_cast<quint8>(damaged[i]) ^ 0x01U);
        EXPECT_FALSE(AnalysisCache::decodeQuality(damaged).ok()) << "header byte " << i;
    }
    for (const qsizetype length : {qsizetype{0}, qsizetype{4}, qsizetype{63}, qsizetype{64}, blob.size() - 1}) {
        EXPECT_FALSE(AnalysisCache::decodeQuality(blob.left(length)).ok()) << length;
    }
    QByteArray longer = blob;
    longer.append('x');
    EXPECT_FALSE(AnalysisCache::decodeQuality(longer).ok());
    // Other kinds of blob, and noise.
    const QByteArray detail = AnalysisCache::encodeDetail(syntheticWaveform(5, 1));
    EXPECT_FALSE(AnalysisCache::decodeQuality(detail).ok());
    EXPECT_FALSE(AnalysisCache::decodeDetail(blob).ok());
    EXPECT_FALSE(AnalysisCache::decodeOverview(blob).ok());
    EXPECT_FALSE(AnalysisCache::decodeQuality(randomBytes(5000, 9)).ok());
}

TEST(AnalysisCacheBlobs, QualityFieldsAreCheckedBehindTheChecksum)
{
    const QByteArray blob = AnalysisCache::encodeQuality(syntheticReport(10, 8));
    ASSERT_TRUE(AnalysisCache::decodeQuality(resealed(blob)).ok());
    const QByteArray brief = AnalysisCache::encodeQuality(syntheticReport(-1));
    const auto refused = [](const QByteArray &changed, const char *what) {
        const Result<QualityReport> result = AnalysisCache::decodeQuality(resealed(changed));
        EXPECT_FALSE(result.ok()) << what;
        if (!result.ok()) {
            EXPECT_EQ(result.error().code, ErrorCode::Corrupt) << what;
        }
    };
    refused(withLe16(blob, 4, 2), "blob version");
    refused(withLe16(blob, 6, kQualityVersion + 1), "quality version");
    refused(withLe32(blob, 8, 0), "no sample rate");
    refused(withLe32(blob, 8, 0x80000000U), "negative sample rate");
    refused(withLe32(blob, 12, 0x1FU), "unknown flag"); // the four known ones and bit 4
    refused(withLe32(blob, 16, 22051), "cut-off above Nyquist");
    refused(withLe32(blob, 16, static_cast<quint32>(-2)), "cut-off below -1");
    refused(withLe32(blob, 20, 0), "flagged kbps of 0");
    refused(withLe32(withLe32(blob, 12, 0x0DU), 20, 7), "kbps without its flag");
    refused(withLe16(blob, 28, 65), "bit depth");
    refused(withLe32(blob, 32, 30000), "content bandwidth above Nyquist");
    QByteArray findings = blob;
    findings[36] = 6;
    refused(findings, "six findings");
    findings = blob;
    findings[38] = findings[37]; // the same finding twice
    refused(findings, "duplicate finding");
    findings = blob;
    findings[37] = 5;
    refused(findings, "unknown finding");
    findings = blob;
    findings[40] = 1;
    refused(findings, "a finding past the count");
    refused(withLe16(blob, 46, 1), "reserved");
    refused(withLe16(blob, 44, 0), "no bins");
    refused(withLe16(blob, 44, 9), "bins that don't match the levels");
    refused(withLe16(blob, 42, 11), "columns that don't match the levels");
    refused(withLe32(withLe32(blob, 48, 5), 52, 0), "fewer frames than columns");
    refused(withLe32(blob, 52, 0x10000U), "more than 24 hours");
    refused(withLe32(brief, 12, 0x07U), "a spectrogram flag without one");
    refused(withLe16(brief, 42, 3), "columns without a spectrogram");
    refused(withLe32(brief, 12, 0x0EU), "compressed nothing");
}

TEST(AnalysisCacheBlobs, QualityHostileSizesDoNotAllocate)
{
    // A header announcing the largest grid over a tiny payload: inflating stops at the declared size, which is
    // bounded by kSpectrogramMaxColumns × kSpectrogramMaxBins.
    const QByteArray blob = AnalysisCache::encodeQuality(syntheticReport(10, 8));
    QByteArray huge = withLe16(withLe16(blob, 42, kSpectrogramMaxColumns), 44, kSpectrogramMaxBins);
    EXPECT_FALSE(AnalysisCache::decodeQuality(resealed(huge)).ok());
    // Beyond the bounds: refused before anything is allocated.
    huge = withLe16(withLe16(blob, 42, 0xFFFFU), 44, 0xFFFFU);
    EXPECT_FALSE(AnalysisCache::decodeQuality(resealed(huge)).ok());
    // A payload size that disagrees with the blob.
    EXPECT_FALSE(AnalysisCache::decodeQuality(resealed(withLe32(blob, 56, 0x7FFFFFFFU))).ok());
    // A bomb: a payload inflating far beyond the declared grid.
    const QualityReport bomb = syntheticReport(10, 8);
    const QByteArray zeros(qsizetype{4} * 1024 * 1024, '\0');
    uLongf packedSize = compressBound(static_cast<uLong>(zeros.size()));
    QByteArray packed(static_cast<qsizetype>(packedSize), Qt::Uninitialized);
    ASSERT_EQ(compress2(reinterpret_cast<Bytef *>(packed.data()), &packedSize,
                        reinterpret_cast<const Bytef *>(zeros.constData()), static_cast<uLong>(zeros.size()), 9),
              Z_OK);
    packed.truncate(static_cast<qsizetype>(packedSize));
    QByteArray bombBlob = AnalysisCache::encodeQuality(bomb).left(56);
    bombBlob = withLe32(bombBlob + QByteArray(4, '\0'), 56, static_cast<quint32>(packed.size())) + packed;
    EXPECT_FALSE(AnalysisCache::decodeQuality(resealed(bombBlob + QByteArray(4, '\0'))).ok());
}

TEST(AnalysisCacheBlobs, UnstorableReportsAreNotEncoded)
{
    QualityReport noRate = syntheticReport(-1);
    noRate.sampleRate = 0;
    EXPECT_TRUE(AnalysisCache::encodeQuality(noRate).isEmpty());
    QualityReport otherRate = syntheticReport(10, 8);
    otherRate.sampleRate = 48000; // the spectrogram says 44.1 kHz
    EXPECT_TRUE(AnalysisCache::encodeQuality(otherRate).isEmpty());
    QualityReport beyond = syntheticReport(-1);
    beyond.cutoffHz = 30000;
    EXPECT_TRUE(AnalysisCache::encodeQuality(beyond).isEmpty());
    QualityReport many = syntheticReport(-1);
    many.findings = QList<QualityFinding>(6, QualityFinding::LikelyFromLossy);
    EXPECT_TRUE(AnalysisCache::encodeQuality(many).isEmpty());
    QualityReport nullGrid = syntheticReport(-1);
    nullGrid.spectrogram = Spectrogram();
    EXPECT_TRUE(AnalysisCache::encodeQuality(nullGrid).isEmpty());
}

TEST(AnalysisCache, QualityTierStoresLoadsAndEvicts)
{
    const QTemporaryDir dir;
    const qint64 reportSize = AnalysisCache::encodeQuality(syntheticReport(800, 256, 1)).size();
    // Room for two reports; waveforms untouched.
    AnalysisCache cache(dir.path(), AnalysisCache::kDefaultDetailBudgetBytes,
                        AnalysisCache::kDefaultOverviewBudgetBytes, (reportSize * 2) + 100);
    EXPECT_FALSE(cache.loadQuality(keyFor(1)));
    ASSERT_TRUE(cache.storeQuality(keyFor(1), syntheticReport(800, 256, 1)).ok());
    EXPECT_EQ(cache.loadQuality(keyFor(1)), syntheticReport(800, 256, 1));
    EXPECT_FALSE(cache.load(keyFor(1))); // no waveform under that key
    ASSERT_TRUE(cache.store(keyFor(1), syntheticWaveform(30, 1)).ok());
    ASSERT_TRUE(cache.storeQuality(keyFor(2), syntheticReport(800, 256, 2)).ok());
    ASSERT_TRUE(cache.loadQuality(keyFor(1))); // 1 is now the most recent
    ASSERT_TRUE(cache.storeQuality(keyFor(3), syntheticReport(800, 256, 3)).ok());
    EXPECT_LE(cache.qualityBytes(), (reportSize * 2) + 100);
    EXPECT_FALSE(cache.loadQuality(keyFor(2))); // least recently used
    EXPECT_FALSE(QFile::exists(qualityPath(dir, keyFor(2))));
    EXPECT_EQ(cache.loadQuality(keyFor(1)), syntheticReport(800, 256, 1));
    EXPECT_EQ(cache.loadQuality(keyFor(3)), syntheticReport(800, 256, 3));
    EXPECT_TRUE(cache.load(keyFor(1))->hasDetail()); // the waveform tiers are separate

    // Storing again replaces the report (and its size).
    ASSERT_TRUE(cache.storeQuality(keyFor(3), syntheticReport(-1)).ok());
    EXPECT_EQ(cache.loadQuality(keyFor(3)), syntheticReport(-1));
    EXPECT_EQ(cache.qualityBytes(), reportSize + 64);
}

TEST(AnalysisCache, AReportLargerThanTheBudgetIsNotKept)
{
    const QTemporaryDir dir;
    AnalysisCache cache(dir.path(), AnalysisCache::kDefaultDetailBudgetBytes,
                        AnalysisCache::kDefaultOverviewBudgetBytes, 1000);
    ASSERT_TRUE(cache.storeQuality(keyFor(1), syntheticReport(800, 256)).ok());
    EXPECT_FALSE(cache.loadQuality(keyFor(1)));
    EXPECT_EQ(cache.qualityBytes(), 0);
    ASSERT_TRUE(cache.storeQuality(keyFor(2), syntheticReport(-1)).ok()); // 64 bytes fit
    EXPECT_TRUE(cache.loadQuality(keyFor(2)));
    AnalysisCache none(dir.filePath(u"none"_s), 0, 0, 0);
    ASSERT_TRUE(none.storeQuality(keyFor(1), syntheticReport(-1)).ok());
    EXPECT_FALSE(none.loadQuality(keyFor(1)));
}

TEST(AnalysisCache, QualityTierSurvivesRestartsAndDiscardsDamage)
{
    const QTemporaryDir dir;
    {
        AnalysisCache cache(dir.path());
        ASSERT_TRUE(cache.storeQuality(keyFor(1), syntheticReport(100, 64, 1)).ok());
        ASSERT_TRUE(cache.storeQuality(keyFor(2), syntheticReport(100, 64, 2)).ok());
        ASSERT_TRUE(cache.storeQuality(keyFor(3), syntheticReport(-1)).ok());
    }
    AnalysisCache cache(dir.path());
    EXPECT_EQ(cache.loadQuality(keyFor(1)), syntheticReport(100, 64, 1));
    // Damaged on disk: a miss, and the file goes.
    QFile damaged(qualityPath(dir, keyFor(2)));
    ASSERT_TRUE(damaged.open(QIODevice::ReadWrite));
    damaged.seek(70);
    damaged.write("garbage");
    damaged.close();
    EXPECT_FALSE(cache.loadQuality(keyFor(2)));
    EXPECT_FALSE(QFile::exists(qualityPath(dir, keyFor(2))));
    // Oversized on disk: a miss without reading it all, and the file goes.
    QFile oversized(qualityPath(dir, keyFor(3)));
    ASSERT_TRUE(oversized.open(QIODevice::WriteOnly));
    ASSERT_TRUE(oversized.resize(qint64{16} * 1024 * 1024));
    oversized.close();
    EXPECT_FALSE(cache.loadQuality(keyFor(3)));
    EXPECT_FALSE(QFile::exists(qualityPath(dir, keyFor(3))));
    EXPECT_GT(cache.totalBytes(), 0);
}

TEST(AnalysisCache, QualityCountsInTheTotalsAndEmpties)
{
    const QTemporaryDir dir;
    {
        AnalysisCache cache(dir.path());
        ASSERT_TRUE(cache.store(keyFor(1), syntheticWaveform(60, 1)).ok());
        ASSERT_TRUE(cache.storeQuality(keyFor(1), syntheticReport(800, 256)).ok());
        ASSERT_TRUE(cache.storeQuality(keyFor(2), syntheticReport(-1)).ok());
        EXPECT_EQ(cache.budgetBytes(), AnalysisCache::kDefaultDetailBudgetBytes +
                                           AnalysisCache::kDefaultOverviewBudgetBytes +
                                           AnalysisCache::kDefaultQualityBudgetBytes);
    }
    // A new process: the sizes come from the folder.
    AnalysisCache cache(dir.path());
    const qint64 total = cache.totalBytes();
    EXPECT_EQ(cache.qualityBytes(), AnalysisCache::encodeQuality(syntheticReport(800, 256)).size() + 64);
    EXPECT_EQ(total, cache.detailBytes() + cache.overviewBytes() + cache.qualityBytes());
    // remove() takes every tier of one key.
    cache.remove(keyFor(1));
    EXPECT_FALSE(cache.load(keyFor(1)));
    EXPECT_FALSE(cache.loadQuality(keyFor(1)));
    EXPECT_EQ(cache.qualityBytes(), 64);
    cache.clear();
    EXPECT_EQ(cache.totalBytes(), 0);
    EXPECT_FALSE(cache.loadQuality(keyFor(2)));
    EXPECT_TRUE(QDir(dir.path() + u"/quality"_s).entryList(QDir::Files).isEmpty());
    ASSERT_TRUE(cache.storeQuality(keyFor(3), syntheticReport(-1)).ok());
    EXPECT_TRUE(cache.loadQuality(keyFor(3)));
}

TEST(AnalysisCache, QualityRefusesNullKeysAndUnwritableFolders)
{
    const QTemporaryDir dir;
    AnalysisCache cache(dir.path());
    EXPECT_EQ(cache.storeQuality(CacheKey{}, syntheticReport(-1)).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(cache.storeQuality(keyFor(1), QualityReport{}).error().code, ErrorCode::InvalidArgument);
    QualityReport beyond = syntheticReport(-1);
    beyond.cutoffHz = 99999;
    EXPECT_EQ(cache.storeQuality(keyFor(1), beyond).error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(cache.loadQuality(CacheKey{}));

    const QString blocker = dir.filePath(u"file"_s);
    QFile file(blocker);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    AnalysisCache blocked(blocker + u"/cache"_s);
    EXPECT_EQ(blocked.storeQuality(keyFor(1), syntheticReport(-1)).error().code, ErrorCode::WriteFailed);
}

TEST(AnalysisCache, RealQualityReportRoundTrip)
{
    const QTemporaryDir dir;
    const auto source = std::make_shared<MemorySource>(makeFlac(quantised(tones({60, 1000, 8000}, 6.0), 16)));
    QualityOptions options;
    options.spectrogram = true;
    const Result<QualityReport> report = analyzeQuality(source, options);
    ASSERT_TRUE(report.ok());
    ASSERT_TRUE(report.value().spectrogram);
    AnalysisCache cache(dir.path());
    const CacheKey key = CacheKey::forContent(*source).value();
    ASSERT_TRUE(cache.storeQuality(key, report.value()).ok());
    EXPECT_EQ(cache.loadQuality(key), report.value());
}
