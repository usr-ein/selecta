// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/AnalysisCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cstring>
#include <limits>
#include <zlib.h>

using namespace Qt::StringLiterals;

namespace selecta::audio {
namespace {

constexpr qint64 kKiB = 1024;
constexpr qint64 kHeadTailBytes = 256 * kKiB;
constexpr int kSampleBlocks = 32;
constexpr qint64 kSampleBlockBytes = 4 * kKiB;

constexpr quint16 kBlobVersion = 1;
constexpr int kHeaderBytes = 36;
constexpr int kCrcBytes = 4;
constexpr int kDetailColumnBytes = 6;
constexpr int kOverviewColumnBytes = 3;
constexpr quint16 kFlagCompressed = 1;
constexpr int kMaxSampleRate = 768000;
constexpr qint64 kMaxSeconds = qint64{24} * 3600;
constexpr qint64 kMaxColumns = kMaxSeconds * kColumnsPerSecond;
constexpr qint64 kMaxDetailBlobBytes = kKiB * 1024 * 128;
constexpr std::string_view kDetailMagic = "SLWD";
constexpr std::string_view kOverviewMagic = "SLWO";
constexpr auto kOverviewSuffix = ".swo"_L1;
constexpr auto kDetailSuffix = ".swd"_L1;

// Quality reports (see DESIGN.md, "Cache format"): a fixed 60-byte header, the spectrogram's levels, a CRC-32.
constexpr std::string_view kQualityMagic = "SLQR";
constexpr auto kQualitySuffix = ".sqr"_L1;
constexpr int kQualityHeaderBytes = 60;
constexpr quint32 kQualitySpectrogram = 1U << 0U;
constexpr quint32 kQualityLikelyKbps = 1U << 1U;
constexpr quint32 kQualityLameLowpass = 1U << 2U;
constexpr quint32 kQualityCompressed = 1U << 3U;
constexpr quint32 kQualityKnownFlags =
    kQualitySpectrogram | kQualityLikelyKbps | kQualityLameLowpass | kQualityCompressed;
constexpr int kFindingSlots = 5;
static_assert(static_cast<int>(QualityFinding::LikelyUpsampled) + 1 == kFindingSlots, "one slot per finding");
constexpr int kMaxBitDepth = 64;
constexpr int kMaxKbpsOrHz = 1000000;
constexpr qint64 kMaxSpectrogramBytes = qint64{kSpectrogramMaxColumns} * kSpectrogramMaxBins;
constexpr qint64 kMaxQualityBlobBytes = (2 * kMaxSpectrogramBytes) + kQualityHeaderBytes + kCrcBytes;

void putLe16(QByteArray &out, quint32 v)
{
    out.append(static_cast<char>(v & 0xFFU));
    out.append(static_cast<char>((v >> 8U) & 0xFFU));
}
void putLe32(QByteArray &out, quint32 v)
{
    putLe16(out, v & 0xFFFFU);
    putLe16(out, v >> 16U);
}
void putLe64(QByteArray &out, quint64 v)
{
    putLe32(out, static_cast<quint32>(v & 0xFFFFFFFFU));
    putLe32(out, static_cast<quint32>(v >> 32U));
}

quint32 getLe16(QByteArrayView b, qsizetype o)
{
    return static_cast<quint32>(static_cast<quint8>(b[o])) |
           (static_cast<quint32>(static_cast<quint8>(b[o + 1])) << 8U);
}
quint32 getLe32(QByteArrayView b, qsizetype o)
{
    return getLe16(b, o) | (getLe16(b, o + 2) << 16U);
}
quint64 getLe64(QByteArrayView b, qsizetype o)
{
    return quint64{getLe32(b, o)} | (quint64{getLe32(b, o + 4)} << 32U);
}

quint32 crcOf(QByteArrayView bytes)
{
    return static_cast<quint32>(
        crc32(0, reinterpret_cast<const Bytef *>(bytes.constData()), static_cast<uInt>(bytes.size())));
}

QByteArray header(std::string_view magic, const Waveform &waveform, quint32 columns, quint16 flags,
                  quint32 payloadBytes)
{
    QByteArray out;
    out.reserve(kHeaderBytes);
    out.append(magic.data(), static_cast<qsizetype>(magic.size()));
    putLe16(out, kBlobVersion);
    putLe16(out, kAnalysisVersion);
    putLe32(out, static_cast<quint32>(waveform.sampleRate()));
    putLe64(out, static_cast<quint64>(waveform.frameCount()));
    putLe16(out, kColumnsPerSecond);
    putLe16(out, static_cast<quint32>(kLowMidCrossoverHz));
    putLe16(out, static_cast<quint32>(kMidHighCrossoverHz));
    putLe16(out, flags);
    putLe32(out, columns);
    putLe32(out, payloadBytes);
    return out;
}

QByteArray seal(QByteArray blob)
{
    putLe32(blob, crcOf(blob));
    return blob;
}

struct BlobHeader
{
    int sampleRate = 0;
    qint64 frameCount = 0;
    quint32 columns = 0;
    quint16 flags = 0;
    QByteArrayView payload;
};

Result<BlobHeader> parseBlob(QByteArrayView blob, std::string_view magic)
{
    if (blob.size() < kHeaderBytes + kCrcBytes) {
        return makeError(ErrorCode::Corrupt, u"cache blob too short"_s);
    }
    if (std::memcmp(blob.constData(), magic.data(), 4) != 0) {
        return makeError(ErrorCode::Corrupt, u"not a waveform cache blob"_s);
    }
    if (getLe16(blob, 4) != kBlobVersion || getLe16(blob, 6) != kAnalysisVersion) {
        return makeError(ErrorCode::Corrupt, u"cache blob from another version"_s);
    }
    if (getLe16(blob, 20) != kColumnsPerSecond || getLe16(blob, 22) != static_cast<quint32>(kLowMidCrossoverHz) ||
        getLe16(blob, 24) != static_cast<quint32>(kMidHighCrossoverHz)) {
        return makeError(ErrorCode::Corrupt, u"cache blob made with other analysis parameters"_s);
    }
    const quint32 stored = getLe32(blob, blob.size() - kCrcBytes);
    if (crcOf(blob.first(blob.size() - kCrcBytes)) != stored) {
        return makeError(ErrorCode::Corrupt, u"cache blob checksum mismatch"_s);
    }
    BlobHeader h;
    const quint32 rate = getLe32(blob, 8);
    const quint64 frames = getLe64(blob, 12);
    if (rate == 0 || rate > kMaxSampleRate || frames > static_cast<quint64>(kMaxSeconds) * rate) {
        return makeError(ErrorCode::Corrupt, u"invalid rate or length in cache blob"_s);
    }
    h.sampleRate = static_cast<int>(rate);
    h.frameCount = static_cast<qint64>(frames);
    h.flags = static_cast<quint16>(getLe16(blob, 26));
    h.columns = getLe32(blob, 28);
    const quint32 payloadBytes = getLe32(blob, 32);
    if (h.columns > kMaxColumns || payloadBytes != static_cast<quint64>(blob.size()) - kHeaderBytes - kCrcBytes) {
        return makeError(ErrorCode::Corrupt, u"invalid sizes in cache blob"_s);
    }
    h.payload = blob.sliced(kHeaderBytes, payloadBytes);
    return h;
}

QString keyFileName(const CacheKey &key, QLatin1StringView suffix)
{
    return key.toHex() + suffix;
}

// A blob file's bytes: nullopt when there is none, empty when it is larger than `maxBytes` (treated as corrupt).
std::optional<QByteArray> readBlob(const QString &path, qint64 maxBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    if (file.size() > maxBytes) {
        return QByteArray();
    }
    return file.readAll();
}

// Recency for the next session's scan.
void touchFile(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    }
}

bool inRange(qint64 value, qint64 low, qint64 high)
{
    return value >= low && value <= high;
}

// The scalar fields a quality blob can hold, checked the same way on both sides of the codec.
bool storableQuality(const QualityReport &r)
{
    const int nyquist = r.sampleRate / 2;
    return inRange(r.sampleRate, 1, kMaxSampleRate) && inRange(r.cutoffHz, -1, nyquist) &&
           inRange(r.contentBandwidthHz, -1, nyquist) && inRange(r.sourceBitDepth, 0, kMaxBitDepth) &&
           inRange(r.effectiveBitDepth, 0, kMaxBitDepth) && inRange(r.likelySourceKbps.value_or(1), 1, kMaxKbpsOrHz) &&
           inRange(r.lameLowpassHz.value_or(1), 1, kMaxKbpsOrHz) && r.findings.size() <= kFindingSlots;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// CacheKey

Result<CacheKey> CacheKey::forContent(ByteSource &source)
{
    const SourceSnapshot snapshot = source.snapshot();
    if (!snapshot.isComplete()) {
        return makeError(ErrorCode::InvalidArgument, u"content keys need a complete source"_s);
    }
    const qint64 size = snapshot.available;
    QCryptographicHash hash(QCryptographicHash::Blake2b_256);
    QByteArray sizeBytes;
    putLe64(sizeBytes, static_cast<quint64>(size));
    hash.addData(sizeBytes);
    QByteArray buffer;
    const auto add = [&](qint64 position, qint64 count) -> bool {
        if (count <= 0) {
            return true;
        }
        buffer.resize(count);
        if (source.read(position, buffer.data(), count) != count) {
            return false;
        }
        hash.addData(buffer);
        return true;
    };
    bool ok = add(0, std::min(size, kHeadTailBytes));
    if (size > 2 * kHeadTailBytes) {
        const qint64 middle = size - (2 * kHeadTailBytes);
        for (int i = 0; ok && i < kSampleBlocks; ++i) {
            const qint64 offset =
                kHeadTailBytes + ((i * std::max<qint64>(0, middle - kSampleBlockBytes)) / (kSampleBlocks - 1));
            ok = add(offset, std::min(kSampleBlockBytes, middle));
        }
    }
    if (ok && size > kHeadTailBytes) {
        const qint64 tailStart = std::max(kHeadTailBytes, size - kHeadTailBytes);
        ok = add(tailStart, size - tailStart);
    }
    if (!ok) {
        return makeError(ErrorCode::ReadFailed, source.lastError().message);
    }
    const QByteArray digest = hash.result();
    CacheKey key;
    std::copy_n(digest.constData(), std::min<qsizetype>(digest.size(), 32), key.m_bytes.begin());
    return key;
}

CacheKey CacheKey::forName(QStringView name)
{
    QCryptographicHash hash(QCryptographicHash::Blake2b_256);
    hash.addData(QByteArrayView("selecta-preview-key:"));
    hash.addData(name.toUtf8());
    const QByteArray digest = hash.result();
    CacheKey key;
    std::copy_n(digest.constData(), std::min<qsizetype>(digest.size(), 32), key.m_bytes.begin());
    return key;
}

std::optional<CacheKey> CacheKey::fromHex(QStringView hex)
{
    if (hex.size() != 64) {
        return std::nullopt;
    }
    for (const QChar c : hex) {
        if (!c.isDigit() && !(c >= u'a' && c <= u'f') && !(c >= u'A' && c <= u'F')) {
            return std::nullopt;
        }
    }
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.size() != 32) {
        return std::nullopt;
    }
    CacheKey key;
    std::copy_n(bytes.constData(), 32, key.m_bytes.begin());
    return key;
}

bool CacheKey::isNull() const
{
    return std::ranges::all_of(m_bytes, [](quint8 b) { return b == 0; });
}

QString CacheKey::toHex() const
{
    return QString::fromLatin1(bytes().toHex());
}

QByteArray CacheKey::bytes() const
{
    return {reinterpret_cast<const char *>(m_bytes.data()), static_cast<qsizetype>(m_bytes.size())};
}

size_t qHash(const CacheKey &key, size_t seed) noexcept
{
    return qHash(key.bytes(), seed);
}

// ---------------------------------------------------------------------------------------------------------------
// Blob codecs

QByteArray AnalysisCache::encodeDetail(const Waveform &waveform)
{
    const QList<WaveformColumn> &detail = waveform.detail();
    QByteArray raw;
    raw.reserve(detail.size() * kDetailColumnBytes);
    for (const WaveformColumn &column : detail) {
        for (const quint8 v : column.peak) {
            raw.append(static_cast<char>(v));
        }
        for (const quint8 v : column.rms) {
            raw.append(static_cast<char>(v));
        }
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    QByteArray compressed(static_cast<qsizetype>(compressedSize), Qt::Uninitialized);
    if (compress2(reinterpret_cast<Bytef *>(compressed.data()), &compressedSize,
                  reinterpret_cast<const Bytef *>(raw.constData()), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        return {};
    }
    compressed.truncate(static_cast<qsizetype>(compressedSize));
    QByteArray blob = header(kDetailMagic, waveform, static_cast<quint32>(detail.size()), kFlagCompressed,
                             static_cast<quint32>(compressed.size()));
    blob.append(compressed);
    return seal(std::move(blob));
}

Result<Waveform> AnalysisCache::decodeDetail(QByteArrayView blob)
{
    const Result<BlobHeader> parsed = parseBlob(blob, kDetailMagic);
    if (!parsed.ok()) {
        return parsed.error();
    }
    const BlobHeader &h = parsed.value();
    if (h.columns != columnCountForFrames(h.frameCount, h.sampleRate)) {
        return makeError(ErrorCode::Corrupt, u"column count doesn't match the length"_s);
    }
    const qsizetype rawSize = static_cast<qsizetype>(h.columns) * kDetailColumnBytes;
    QByteArray raw;
    if ((h.flags & kFlagCompressed) != 0) {
        raw.resize(rawSize);
        auto rawLength = static_cast<uLongf>(rawSize);
        const int result =
            uncompress(reinterpret_cast<Bytef *>(raw.data()), &rawLength,
                       reinterpret_cast<const Bytef *>(h.payload.constData()), static_cast<uLong>(h.payload.size()));
        if (result != Z_OK || std::cmp_not_equal(rawLength, rawSize)) {
            return makeError(ErrorCode::Corrupt, u"detail payload doesn't inflate to its declared size"_s);
        }
    } else {
        if (h.payload.size() != rawSize) {
            return makeError(ErrorCode::Corrupt, u"detail payload of the wrong size"_s);
        }
        raw = h.payload.toByteArray();
    }
    QList<WaveformColumn> detail;
    detail.resize(h.columns);
    for (qsizetype i = 0; i < detail.size(); ++i) {
        WaveformColumn &column = detail[i];
        const qsizetype base = i * kDetailColumnBytes;
        for (std::size_t b = 0; b < kBandCount; ++b) {
            column.peak.at(b) = static_cast<quint8>(raw[base + static_cast<qsizetype>(b)]);
            column.rms.at(b) = static_cast<quint8>(raw[base + kBandCount + static_cast<qsizetype>(b)]);
        }
    }
    return Waveform::fromDetail(h.sampleRate, h.frameCount, std::move(detail), true);
}

QByteArray AnalysisCache::encodeOverview(const Waveform &waveform)
{
    QByteArray payload;
    payload.reserve(qsizetype{kOverviewColumns} * kOverviewColumnBytes);
    for (const OverviewColumn &column : waveform.overview()) {
        for (const quint8 v : column.peak) {
            payload.append(static_cast<char>(v));
        }
    }
    QByteArray blob = header(kOverviewMagic, waveform, static_cast<quint32>(waveform.overview().size()), 0,
                             static_cast<quint32>(payload.size()));
    blob.append(payload);
    return seal(std::move(blob));
}

Result<Waveform> AnalysisCache::decodeOverview(QByteArrayView blob)
{
    const Result<BlobHeader> parsed = parseBlob(blob, kOverviewMagic);
    if (!parsed.ok()) {
        return parsed.error();
    }
    const BlobHeader &h = parsed.value();
    if (h.columns != kOverviewColumns || h.flags != 0 ||
        h.payload.size() != qsizetype{kOverviewColumns} * kOverviewColumnBytes) {
        return makeError(ErrorCode::Corrupt, u"invalid overview payload"_s);
    }
    QList<OverviewColumn> overview;
    overview.resize(kOverviewColumns);
    for (qsizetype i = 0; i < overview.size(); ++i) {
        for (std::size_t b = 0; b < kBandCount; ++b) {
            overview[i].peak.at(b) =
                static_cast<quint8>(h.payload[(i * kOverviewColumnBytes) + static_cast<qsizetype>(b)]);
        }
    }
    return Waveform::fromOverview(h.sampleRate, h.frameCount, std::move(overview));
}

QByteArray AnalysisCache::encodeQuality(const QualityReport &report)
{
    if (!storableQuality(report)) {
        return {};
    }
    quint32 flags = 0;
    flags |= report.likelySourceKbps ? kQualityLikelyKbps : 0U;
    flags |= report.lameLowpassHz ? kQualityLameLowpass : 0U;
    QByteArray payload;
    const Spectrogram *spectrogram = report.spectrogram ? &*report.spectrogram : nullptr;
    if (spectrogram != nullptr) {
        if (spectrogram->isNull() || spectrogram->sampleRate() != report.sampleRate) {
            return {};
        }
        flags |= kQualitySpectrogram;
        const QByteArray &levels = spectrogram->levels();
        if (!levels.isEmpty()) {
            uLongf compressedSize = compressBound(static_cast<uLong>(levels.size()));
            payload.resize(static_cast<qsizetype>(compressedSize));
            if (compress2(reinterpret_cast<Bytef *>(payload.data()), &compressedSize,
                          reinterpret_cast<const Bytef *>(levels.constData()), static_cast<uLong>(levels.size()),
                          6) != Z_OK) {
                return {};
            }
            payload.truncate(static_cast<qsizetype>(compressedSize));
            flags |= kQualityCompressed;
        }
    }
    QByteArray blob;
    blob.reserve(kQualityHeaderBytes + payload.size() + kCrcBytes);
    blob.append(kQualityMagic.data(), static_cast<qsizetype>(kQualityMagic.size()));
    putLe16(blob, kBlobVersion);
    putLe16(blob, kQualityVersion);
    putLe32(blob, static_cast<quint32>(report.sampleRate));
    putLe32(blob, flags);
    putLe32(blob, static_cast<quint32>(report.cutoffHz)); // signed, two's complement
    putLe32(blob, static_cast<quint32>(report.likelySourceKbps.value_or(0)));
    putLe32(blob, static_cast<quint32>(report.lameLowpassHz.value_or(0)));
    putLe16(blob, static_cast<quint32>(report.sourceBitDepth));
    putLe16(blob, static_cast<quint32>(report.effectiveBitDepth));
    putLe32(blob, static_cast<quint32>(report.contentBandwidthHz));
    blob.append(static_cast<char>(report.findings.size()));
    for (int i = 0; i < kFindingSlots; ++i) {
        blob.append(i < report.findings.size() ? static_cast<char>(report.findings[i]) : '\0');
    }
    putLe16(blob, spectrogram != nullptr ? static_cast<quint32>(spectrogram->columns()) : 0U);
    putLe16(blob, spectrogram != nullptr ? static_cast<quint32>(spectrogram->bins()) : 0U);
    putLe16(blob, 0); // reserved
    putLe64(blob, spectrogram != nullptr ? static_cast<quint64>(spectrogram->frameCount()) : 0U);
    putLe32(blob, static_cast<quint32>(payload.size()));
    blob.append(payload);
    return seal(std::move(blob));
}

Result<QualityReport> AnalysisCache::decodeQuality(QByteArrayView blob)
{
    if (blob.size() < kQualityHeaderBytes + kCrcBytes) {
        return makeError(ErrorCode::Corrupt, u"quality blob too short"_s);
    }
    if (std::memcmp(blob.constData(), kQualityMagic.data(), 4) != 0) {
        return makeError(ErrorCode::Corrupt, u"not a quality cache blob"_s);
    }
    if (getLe16(blob, 4) != kBlobVersion || getLe16(blob, 6) != kQualityVersion) {
        return makeError(ErrorCode::Corrupt, u"quality blob from another version"_s);
    }
    if (crcOf(blob.first(blob.size() - kCrcBytes)) != getLe32(blob, blob.size() - kCrcBytes)) {
        return makeError(ErrorCode::Corrupt, u"quality blob checksum mismatch"_s);
    }
    const quint32 flags = getLe32(blob, 12);
    const auto asInt = [&blob](qsizetype offset) { return static_cast<qint32>(getLe32(blob, offset)); };
    QualityReport report;
    report.sampleRate = asInt(8);
    report.cutoffHz = asInt(16);
    if ((flags & kQualityLikelyKbps) != 0) {
        report.likelySourceKbps = asInt(20);
    }
    if ((flags & kQualityLameLowpass) != 0) {
        report.lameLowpassHz = asInt(24);
    }
    report.sourceBitDepth = static_cast<int>(getLe16(blob, 28));
    report.effectiveBitDepth = static_cast<int>(getLe16(blob, 30));
    report.contentBandwidthHz = asInt(32);
    const int findingCount = static_cast<quint8>(blob[36]);
    const quint32 columns = getLe16(blob, 42);
    const quint32 bins = getLe16(blob, 44);
    const quint64 frames = getLe64(blob, 48);
    const quint32 payloadBytes = getLe32(blob, 56);
    // Absent optional fields must read 0, as encodeQuality() writes them: anything else is damage.
    const bool unusedZero = ((flags & kQualityLikelyKbps) != 0 || getLe32(blob, 20) == 0) &&
                            ((flags & kQualityLameLowpass) != 0 || getLe32(blob, 24) == 0) && getLe16(blob, 46) == 0;
    if ((flags & ~kQualityKnownFlags) != 0 || !unusedZero || !storableQuality(report) || findingCount > kFindingSlots ||
        payloadBytes != static_cast<quint64>(blob.size()) - kQualityHeaderBytes - kCrcBytes) {
        return makeError(ErrorCode::Corrupt, u"invalid fields in quality blob"_s);
    }
    for (int i = 0; i < kFindingSlots; ++i) {
        const auto value = static_cast<quint8>(blob[37 + i]);
        if (i >= findingCount) {
            if (value != 0) {
                return makeError(ErrorCode::Corrupt, u"invalid findings in quality blob"_s);
            }
            continue;
        }
        const auto finding = static_cast<QualityFinding>(value);
        if (value >= kFindingSlots || report.findings.contains(finding)) {
            return makeError(ErrorCode::Corrupt, u"invalid findings in quality blob"_s);
        }
        report.findings.append(finding);
    }
    const QByteArrayView payload = blob.sliced(kQualityHeaderBytes, payloadBytes);
    if ((flags & kQualitySpectrogram) == 0) {
        if (columns != 0 || bins != 0 || frames != 0 || !payload.isEmpty() || (flags & kQualityCompressed) != 0) {
            return makeError(ErrorCode::Corrupt, u"spectrogram fields without a spectrogram"_s);
        }
        return report;
    }
    // Bounded before anything is allocated: at most kSpectrogramMaxColumns × kSpectrogramMaxBins bytes.
    if (columns > static_cast<quint32>(kSpectrogramMaxColumns) || bins < 1 ||
        bins > static_cast<quint32>(kSpectrogramMaxBins) || frames < columns ||
        frames > static_cast<quint64>(kMaxSeconds) * static_cast<quint64>(report.sampleRate)) {
        return makeError(ErrorCode::Corrupt, u"invalid spectrogram size in quality blob"_s);
    }
    const qsizetype rawSize = static_cast<qsizetype>(columns) * static_cast<qsizetype>(bins);
    QByteArray levels;
    if ((flags & kQualityCompressed) != 0) {
        levels.resize(rawSize);
        auto rawLength = static_cast<uLongf>(rawSize);
        const int result =
            uncompress(reinterpret_cast<Bytef *>(levels.data()), &rawLength,
                       reinterpret_cast<const Bytef *>(payload.constData()), static_cast<uLong>(payload.size()));
        if (result != Z_OK || std::cmp_not_equal(rawLength, rawSize)) {
            return makeError(ErrorCode::Corrupt, u"spectrogram doesn't inflate to its declared size"_s);
        }
    } else {
        if (payload.size() != rawSize) {
            return makeError(ErrorCode::Corrupt, u"spectrogram of the wrong size"_s);
        }
        levels = payload.toByteArray();
    }
    Result<Spectrogram> spectrogram =
        Spectrogram::fromLevels(report.sampleRate, static_cast<qint64>(frames), static_cast<int>(columns),
                                static_cast<int>(bins), std::move(levels));
    if (!spectrogram.ok()) {
        return makeError(ErrorCode::Corrupt, spectrogram.error().message);
    }
    report.spectrogram = spectrogram.takeValue();
    return report;
}

// ---------------------------------------------------------------------------------------------------------------
// AnalysisCache

AnalysisCache::AnalysisCache(QString directory, qint64 detailBudgetBytes, qint64 overviewBudgetBytes,
                             qint64 qualityBudgetBytes)
    : m_directory(std::move(directory))
{
    m_detail.budget = std::max<qint64>(0, detailBudgetBytes);
    m_overview.budget = std::max<qint64>(0, overviewBudgetBytes);
    m_quality.budget = std::max<qint64>(0, qualityBudgetBytes);
}

AnalysisCache::~AnalysisCache() = default;

QString AnalysisCache::folderFor(Tier tier) const
{
    switch (tier) {
    case Tier::Overview:
        return m_directory + u"/overview"_s;
    case Tier::Detail:
        return m_directory + u"/detail"_s;
    case Tier::Quality:
        break;
    }
    return m_directory + u"/quality"_s;
}

QLatin1StringView AnalysisCache::suffixFor(Tier tier)
{
    switch (tier) {
    case Tier::Overview:
        return kOverviewSuffix;
    case Tier::Detail:
        return kDetailSuffix;
    case Tier::Quality:
        break;
    }
    return kQualitySuffix;
}

QString AnalysisCache::pathFor(Tier tier, const CacheKey &key) const
{
    return folderFor(tier) + u'/' + keyFileName(key, suffixFor(tier));
}

AnalysisCache::TierState &AnalysisCache::tierState(Tier tier)
{
    switch (tier) {
    case Tier::Overview:
        return m_overview;
    case Tier::Detail:
        return m_detail;
    case Tier::Quality:
        break;
    }
    return m_quality;
}

void AnalysisCache::ensureScannedLocked()
{
    if (m_scanned) {
        return;
    }
    m_scanned = true;
    for (const Tier tier : kTiers) {
        const QString folder = folderFor(tier);
        const QLatin1StringView suffix = suffixFor(tier);
        // Oldest first, so recency survives restarts through file modification times.
        const QFileInfoList files =
            QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
        TierState &state = tierState(tier);
        for (const QFileInfo &file : files) {
            const QString name = file.fileName();
            if (!name.endsWith(suffix)) {
                continue;
            }
            const std::optional<CacheKey> key = CacheKey::fromHex(QStringView(name).chopped(suffix.size()));
            if (!key || state.entries.contains(*key)) {
                continue;
            }
            state.entries.insert(*key, Entry{file.size(), ++m_useCounter});
            state.bytes += file.size();
        }
    }
}

void AnalysisCache::touchLocked(Tier tier, const CacheKey &key, qint64 bytes)
{
    TierState &state = tierState(tier);
    const auto it = state.entries.find(key);
    if (it != state.entries.end()) {
        state.bytes -= it->bytes;
        it->bytes = bytes;
        it->lastUse = ++m_useCounter;
    } else {
        state.entries.insert(key, Entry{bytes, ++m_useCounter});
    }
    state.bytes += bytes;
}

void AnalysisCache::evictLocked(Tier tier)
{
    TierState &state = tierState(tier);
    while (state.bytes > state.budget && !state.entries.isEmpty()) {
        auto oldest = state.entries.begin();
        for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
            if (it->lastUse < oldest->lastUse) {
                oldest = it;
            }
        }
        QFile::remove(pathFor(tier, oldest.key()));
        state.bytes -= oldest->bytes;
        state.entries.erase(oldest);
    }
}

Result<void> AnalysisCache::writeBlob(Tier tier, const CacheKey &key, const QByteArray &blob)
{
    const QString path = pathFor(tier, key);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return makeError(ErrorCode::WriteFailed, u"can't create %1"_s.arg(QFileInfo(path).absolutePath()));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(blob) != blob.size() || !file.commit()) {
        return makeError(ErrorCode::WriteFailed, u"can't write %1: %2"_s.arg(path, file.errorString()));
    }
    return {};
}

void AnalysisCache::discard(Tier tier, const CacheKey &key)
{
    QFile::remove(pathFor(tier, key));
    const std::scoped_lock lock(m_mutex);
    TierState &state = tierState(tier);
    const auto it = state.entries.find(key);
    if (it != state.entries.end()) {
        state.bytes -= it->bytes;
        state.entries.erase(it);
    }
}

std::optional<Waveform> AnalysisCache::load(const CacheKey &key)
{
    if (key.isNull()) {
        return std::nullopt;
    }
    const QString detailPath = pathFor(Tier::Detail, key);
    if (const std::optional<QByteArray> blob = readBlob(detailPath, kMaxDetailBlobBytes)) {
        Result<Waveform> waveform = decodeDetail(*blob);
        if (waveform.ok()) {
            {
                const std::scoped_lock lock(m_mutex);
                touchLocked(Tier::Detail, key, blob->size());
            }
            touchFile(detailPath);
            return waveform.takeValue();
        }
        qCDebug(lcAudio) << "discarding cached detail:" << waveform.error().toString();
        discard(Tier::Detail, key);
    }
    const QString overviewPath = pathFor(Tier::Overview, key);
    if (const std::optional<QByteArray> blob =
            readBlob(overviewPath, kHeaderBytes + kCrcBytes + (kOverviewColumns * 3))) {
        Result<Waveform> waveform = decodeOverview(*blob);
        if (waveform.ok()) {
            const std::scoped_lock lock(m_mutex);
            touchLocked(Tier::Overview, key, blob->size());
            return waveform.takeValue();
        }
        qCDebug(lcAudio) << "discarding cached overview:" << waveform.error().toString();
        discard(Tier::Overview, key);
    }
    return std::nullopt;
}

Result<void> AnalysisCache::store(const CacheKey &key, const Waveform &waveform)
{
    if (key.isNull() || waveform.isNull() || !waveform.isComplete() || waveform.overview().isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, u"only complete waveforms are cached"_s);
    }
    const QByteArray overview = encodeOverview(waveform);
    if (Result<void> written = writeBlob(Tier::Overview, key, overview); !written.ok()) {
        return written;
    }
    QByteArray detail;
    if (waveform.hasDetail()) {
        detail = encodeDetail(waveform);
        if (detail.isEmpty()) {
            return makeError(ErrorCode::Internal, u"can't compress the waveform detail"_s);
        }
        if (Result<void> written = writeBlob(Tier::Detail, key, detail); !written.ok()) {
            return written;
        }
    }
    const std::scoped_lock lock(m_mutex);
    ensureScannedLocked();
    touchLocked(Tier::Overview, key, overview.size());
    if (!detail.isEmpty()) {
        touchLocked(Tier::Detail, key, detail.size());
    }
    evictLocked(Tier::Detail);
    evictLocked(Tier::Overview);
    return {};
}

std::optional<QualityReport> AnalysisCache::loadQuality(const CacheKey &key)
{
    if (key.isNull()) {
        return std::nullopt;
    }
    const QString path = pathFor(Tier::Quality, key);
    const std::optional<QByteArray> blob = readBlob(path, kMaxQualityBlobBytes);
    if (!blob) {
        return std::nullopt;
    }
    Result<QualityReport> report = decodeQuality(*blob);
    if (!report.ok()) {
        qCDebug(lcAudio) << "discarding cached quality report:" << report.error().toString();
        discard(Tier::Quality, key);
        return std::nullopt;
    }
    {
        const std::scoped_lock lock(m_mutex);
        touchLocked(Tier::Quality, key, blob->size());
    }
    touchFile(path);
    return report.takeValue();
}

Result<void> AnalysisCache::storeQuality(const CacheKey &key, const QualityReport &report)
{
    if (key.isNull() || report.sampleRate <= 0) {
        return makeError(ErrorCode::InvalidArgument, u"only finished quality reports are cached"_s);
    }
    const QByteArray blob = encodeQuality(report);
    if (blob.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, u"this quality report can't be cached"_s);
    }
    if (Result<void> written = writeBlob(Tier::Quality, key, blob); !written.ok()) {
        return written;
    }
    const std::scoped_lock lock(m_mutex);
    ensureScannedLocked();
    touchLocked(Tier::Quality, key, blob.size());
    evictLocked(Tier::Quality);
    return {};
}

void AnalysisCache::remove(const CacheKey &key)
{
    for (const Tier tier : kTiers) {
        QFile::remove(pathFor(tier, key));
    }
    const std::scoped_lock lock(m_mutex);
    for (const Tier tier : kTiers) {
        TierState &state = tierState(tier);
        const auto it = state.entries.find(key);
        if (it != state.entries.end()) {
            state.bytes -= it->bytes;
            state.entries.erase(it);
        }
    }
}

qint64 AnalysisCache::totalBytes()
{
    const std::scoped_lock lock(m_mutex);
    ensureScannedLocked();
    return m_detail.bytes + m_overview.bytes + m_quality.bytes;
}

qint64 AnalysisCache::budgetBytes() const
{
    return m_detail.budget + m_overview.budget + m_quality.budget;
}

void AnalysisCache::clear()
{
    const std::scoped_lock lock(m_mutex);
    ensureScannedLocked();
    for (const Tier tier : kTiers) {
        TierState &state = tierState(tier);
        for (auto it = state.entries.cbegin(); it != state.entries.cend(); ++it) {
            QFile::remove(pathFor(tier, it.key()));
        }
        state.entries.clear();
        state.bytes = 0;
    }
}

qint64 AnalysisCache::detailBytes() const
{
    const std::scoped_lock lock(m_mutex);
    return m_detail.bytes;
}

qint64 AnalysisCache::overviewBytes() const
{
    const std::scoped_lock lock(m_mutex);
    return m_overview.bytes;
}

qint64 AnalysisCache::qualityBytes() const
{
    const std::scoped_lock lock(m_mutex);
    return m_quality.bytes;
}

} // namespace selecta::audio
