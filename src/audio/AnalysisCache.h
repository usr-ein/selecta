// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/Error.h"
#include "audio/QualityCheck.h"
#include "audio/Waveform.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QLatin1StringView>
#include <QString>
#include <QStringView>

#include <array>
#include <mutex>
#include <optional>

namespace selecta::audio {

// Identifies an analysed file in the AnalysisCache: 32 bytes (BLAKE2b-256).
class CacheKey
{
public:
    CacheKey() = default;

    // Content key of a complete source: BLAKE2b-256 of its size, its first and last 256 KiB and 32 blocks of 4 KiB
    // spread evenly between them. Tags living at either end are part of the key, so a re-tagged file is analysed
    // again; that is rare and cheap. Reads at most ~640 KiB, whatever the file size. Failure codes: InvalidArgument
    // (source not complete), ReadFailed.
    static Result<CacheKey> forContent(ByteSource &source);
    // A provisional key for a preview whose content isn't complete yet, e.g. from "peer|remote path|size" (audio
    // §7.3). Store the finished analysis under its content key as well.
    static CacheKey forName(QStringView name);
    // 64 hex digits, as produced by toHex(); nullopt for anything else.
    static std::optional<CacheKey> fromHex(QStringView hex);

    bool isNull() const;
    QString toHex() const;
    QByteArray bytes() const;

    friend bool operator==(const CacheKey &, const CacheKey &) = default;

private:
    std::array<quint8, 32> m_bytes{};
};

size_t qHash(const CacheKey &key, size_t seed = 0) noexcept;

// The analysis cache (PREV-8, PREV-9, CACHE-1, ADR-007): three tiers of small files in a directory the caller owns
// (normally ~/Library/Caches/Selecta/analysis/), each bounded by its own budget and evicting the least recently used
// entries beyond it.
//
// - The overview tier keeps the 3.6 KB waveform overview of every analysed track for a long time, bounded by
//   `overviewBudgetBytes` (default 64 MiB, about 15 000 tracks).
// - The detail tier keeps the full 150 columns/s waveform detail (~0.1–0.3 MB per track, zlib-compressed), bounded by
//   `detailBudgetBytes` (default 256 MiB).
// - The quality tier keeps quality reports (analyzeQuality()) with their spectrogram when they have one, bounded by
//   `qualityBudgetBytes` (default 64 MiB). A report is 64 bytes without a spectrogram; with the default 800 × 256 grid,
//   zlib-compressed, up to ~120 KB for noise-like material (the worst case) and far less for tonal music, so the
//   default keeps at least 550 reports with spectrograms.
//
// Every blob is versioned (format, and kAnalysisVersion or kQualityVersion) and checksummed. A blob that is corrupt,
// truncated, oversized or from another version reads as a miss and is deleted, so the caller simply recomputes.
// Writes go through a temporary file and a rename, so a crash never leaves a half-written entry.
//
// Threading: thread-safe; every member may be called from any thread. File I/O happens on the calling thread, so
// call it from a worker (reads are a few hundred KB at most). Recency is tracked in memory (and in file modification
// times, so it survives restarts); the directory is scanned once, on the first store, totalBytes() or clear().
class AnalysisCache
{
public:
    static constexpr qint64 kDefaultDetailBudgetBytes = qint64{256} * 1024 * 1024;
    static constexpr qint64 kDefaultOverviewBudgetBytes = qint64{64} * 1024 * 1024;
    static constexpr qint64 kDefaultQualityBudgetBytes = qint64{64} * 1024 * 1024;

    explicit AnalysisCache(QString directory, qint64 detailBudgetBytes = kDefaultDetailBudgetBytes,
                           qint64 overviewBudgetBytes = kDefaultOverviewBudgetBytes,
                           qint64 qualityBudgetBytes = kDefaultQualityBudgetBytes);
    ~AnalysisCache();
    AnalysisCache(const AnalysisCache &) = delete;
    AnalysisCache &operator=(const AnalysisCache &) = delete;
    AnalysisCache(AnalysisCache &&) = delete;
    AnalysisCache &operator=(AnalysisCache &&) = delete;

    QString directory() const { return m_directory; }

    // The cached waveform with its detail when the detail tier still has it, else overview only
    // (Waveform::hasDetail() false), else nullopt. A hit refreshes the entry's recency.
    std::optional<Waveform> load(const CacheKey &key);
    // Stores a complete waveform: the overview always, the detail when present. Then evicts least recently used
    // entries beyond the budgets. Partial waveforms are not stored. Failure codes: InvalidArgument, WriteFailed.
    Result<void> store(const CacheKey &key, const Waveform &waveform);
    // The cached quality report, with the spectrogram it was stored with (if any: check report.spectrogram and its
    // dimensions before relying on it), else nullopt. A hit refreshes the entry's recency.
    std::optional<QualityReport> loadQuality(const CacheKey &key);
    // Stores a report, replacing the key's earlier one, then evicts least recently used reports beyond the quality
    // budget (a report larger than the whole budget is evicted at once). Failure codes: InvalidArgument (null key, a
    // report with no sample rate), WriteFailed.
    Result<void> storeQuality(const CacheKey &key, const QualityReport &report);

    // Removes every tier of an entry. Missing entries are not an error.
    void remove(const CacheKey &key);

    // Current sizes of the tiers in bytes (after the first scan; before it, what this process has written).
    qint64 detailBytes() const;
    qint64 overviewBytes() const;
    qint64 qualityBytes() const;
    // Every tier on disk, in bytes; scans the folder the first time (CACHE-2).
    qint64 totalBytes();
    // The sum of the three budgets: the most the cache keeps on disk (CACHE-2's "at most …").
    qint64 budgetBytes() const;
    // Removes every entry of every tier (CACHE-2): what is needed again is analysed again.
    void clear();

    // Blob codecs, public for tests and fuzzing. Decoding validates everything (magic, versions, sizes against
    // bounds, CRC-32) and never allocates more than the bound for the declared column count (detail) or grid
    // (spectrogram: kSpectrogramMaxColumns × kSpectrogramMaxBins bytes).
    static QByteArray encodeDetail(const Waveform &waveform);
    static Result<Waveform> decodeDetail(QByteArrayView blob);
    static QByteArray encodeOverview(const Waveform &waveform);
    static Result<Waveform> decodeOverview(QByteArrayView blob);
    // Empty for a report that can't be stored (no sample rate) or when compression fails.
    static QByteArray encodeQuality(const QualityReport &report);
    static Result<QualityReport> decodeQuality(QByteArrayView blob);

private:
    enum class Tier { Overview, Detail, Quality };
    struct Entry
    {
        qint64 bytes = 0;
        qint64 lastUse = 0; // monotonically increasing use counter, seeded from file times by the scan
    };
    struct TierState
    {
        QHash<CacheKey, Entry> entries;
        qint64 bytes = 0;
        qint64 budget = 0;
    };

    static constexpr std::array<Tier, 3> kTiers = {Tier::Overview, Tier::Detail, Tier::Quality};

    QString folderFor(Tier tier) const;
    static QLatin1StringView suffixFor(Tier tier);
    QString pathFor(Tier tier, const CacheKey &key) const;
    TierState &tierState(Tier tier);
    // Deletes a blob that failed to decode and forgets it.
    void discard(Tier tier, const CacheKey &key);
    void ensureScannedLocked();
    void touchLocked(Tier tier, const CacheKey &key, qint64 bytes);
    void evictLocked(Tier tier);
    Result<void> writeBlob(Tier tier, const CacheKey &key, const QByteArray &blob);

    const QString m_directory;
    mutable std::mutex m_mutex;
    TierState m_overview;
    TierState m_detail;
    TierState m_quality;
    qint64 m_useCounter = 0;
    bool m_scanned = false;
};

} // namespace selecta::audio
