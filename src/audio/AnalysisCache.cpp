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

// ---------------------------------------------------------------------------------------------------------------
// AnalysisCache

AnalysisCache::AnalysisCache(QString directory, qint64 detailBudgetBytes, qint64 overviewBudgetBytes)
    : m_directory(std::move(directory))
{
    m_detail.budget = std::max<qint64>(0, detailBudgetBytes);
    m_overview.budget = std::max<qint64>(0, overviewBudgetBytes);
}

AnalysisCache::~AnalysisCache() = default;

QString AnalysisCache::pathFor(Tier tier, const CacheKey &key) const
{
    return tier == Tier::Detail ? m_directory + u"/detail/"_s + keyFileName(key, kDetailSuffix)
                                : m_directory + u"/overview/"_s + keyFileName(key, kOverviewSuffix);
}

AnalysisCache::TierState &AnalysisCache::tierState(Tier tier)
{
    return tier == Tier::Detail ? m_detail : m_overview;
}

void AnalysisCache::ensureScannedLocked()
{
    if (m_scanned) {
        return;
    }
    m_scanned = true;
    for (const Tier tier : {Tier::Overview, Tier::Detail}) {
        const QString folder = m_directory + (tier == Tier::Detail ? u"/detail"_s : u"/overview"_s);
        const QLatin1StringView suffix = tier == Tier::Detail ? kDetailSuffix : kOverviewSuffix;
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

std::optional<Waveform> AnalysisCache::load(const CacheKey &key)
{
    if (key.isNull()) {
        return std::nullopt;
    }
    const auto readBlob = [](const QString &path, qint64 maxBytes) -> std::optional<QByteArray> {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        if (file.size() > maxBytes) {
            return QByteArray(); // oversized: treated as corrupt
        }
        return file.readAll();
    };
    const auto discard = [this](Tier tier, const CacheKey &k) {
        QFile::remove(pathFor(tier, k));
        const std::scoped_lock lock(m_mutex);
        TierState &state = tierState(tier);
        const auto it = state.entries.find(k);
        if (it != state.entries.end()) {
            state.bytes -= it->bytes;
            state.entries.erase(it);
        }
    };

    const QString detailPath = pathFor(Tier::Detail, key);
    if (const std::optional<QByteArray> blob = readBlob(detailPath, kMaxDetailBlobBytes)) {
        Result<Waveform> waveform = decodeDetail(*blob);
        if (waveform.ok()) {
            {
                const std::scoped_lock lock(m_mutex);
                touchLocked(Tier::Detail, key, blob->size());
            }
            QFile file(detailPath); // recency for the next session's scan
            if (file.open(QIODevice::ReadWrite)) {
                file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
            }
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

void AnalysisCache::remove(const CacheKey &key)
{
    QFile::remove(pathFor(Tier::Detail, key));
    QFile::remove(pathFor(Tier::Overview, key));
    const std::scoped_lock lock(m_mutex);
    for (const Tier tier : {Tier::Overview, Tier::Detail}) {
        TierState &state = tierState(tier);
        const auto it = state.entries.find(key);
        if (it != state.entries.end()) {
            state.bytes -= it->bytes;
            state.entries.erase(it);
        }
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

} // namespace selecta::audio
