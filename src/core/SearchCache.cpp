// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchCache.h"

#include "storage/Database.h"

#include <QDataStream>
#include <QDateTime>
#include <QIODevice>
#include <QLoggingCategory>
#include <QtEndian>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcSearchCache, "selecta.core.searchcache")

using storage::Statement;

constexpr quint32 kMagic = 0x53524348; // "SRCH"
constexpr quint16 kVersion = 1;
constexpr qsizetype kMaxOtherAttributes = 16;

// Bits of the per-hit flags byte.
constexpr quint8 HasBitrate = 1U << 0U;
constexpr quint8 HasDuration = 1U << 1U;
constexpr quint8 HasVbr = 1U << 2U;
constexpr quint8 HasSampleRate = 1U << 3U;
constexpr quint8 HasBitDepth = 1U << 4U;
constexpr quint8 FreeSlot = 1U << 5U;
constexpr quint8 Vbr = 1U << 6U;

} // namespace

SearchCacheStore::SearchCacheStore(storage::Database &db, WallClock now, qint64 lifetimeMs)
    : m_db(db)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_lifetimeMs(lifetimeMs)
{}

QByteArray SearchCacheStore::encode(const QList<SearchHit> &hits)
{
    QByteArray raw;
    {
        QDataStream out(&raw, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_5);
        const qsizetype count = std::min(hits.size(), kMaxHits);
        out << kMagic << kVersion << static_cast<quint32>(count);
        for (qsizetype i = 0; i < count; ++i) {
            const SearchHit &hit = hits.at(i);
            const slsk::FileAttributes &attributes = hit.attributes;
            quint8 flags = 0;
            flags |= attributes.bitrate ? HasBitrate : 0U;
            flags |= attributes.duration ? HasDuration : 0U;
            flags |= attributes.vbr ? HasVbr : 0U;
            flags |= attributes.sampleRate ? HasSampleRate : 0U;
            flags |= attributes.bitDepth ? HasBitDepth : 0U;
            flags |= hit.freeSlot ? FreeSlot : 0U;
            flags |= attributes.vbr.value_or(false) ? Vbr : 0U;
            out << hit.peer << hit.path << static_cast<quint64>(hit.size) << hit.extension << flags
                << attributes.bitrate.value_or(0) << attributes.duration.value_or(0)
                << attributes.sampleRate.value_or(0) << attributes.bitDepth.value_or(0) << hit.queueLength
                << hit.averageSpeed;
            const qsizetype others = std::min(attributes.other.size(), kMaxOtherAttributes);
            out << static_cast<quint8>(others);
            for (qsizetype other = 0; other < others; ++other) {
                out << attributes.other.at(other).first << attributes.other.at(other).second;
            }
        }
    }
    return qCompress(raw);
}

std::optional<QList<SearchHit>> SearchCacheStore::decode(const QByteArray &blob)
{
    // qCompress() puts the inflated size first (big-endian): refuse anything beyond the bound before inflating.
    if (blob.size() < 4 || qFromBigEndian<quint32>(blob.constData()) > static_cast<quint32>(kMaxEncodedBytes)) {
        return std::nullopt;
    }
    const QByteArray raw = qUncompress(blob);
    if (raw.isEmpty()) {
        return std::nullopt;
    }
    QDataStream in(raw);
    in.setVersion(QDataStream::Qt_6_5);
    quint32 magic = 0;
    quint16 version = 0;
    quint32 count = 0;
    in >> magic >> version >> count;
    if (in.status() != QDataStream::Ok || magic != kMagic || version != kVersion ||
        count > static_cast<quint32>(kMaxHits)) {
        return std::nullopt;
    }
    QList<SearchHit> hits;
    hits.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        SearchHit hit;
        quint64 size = 0;
        quint8 flags = 0;
        quint32 bitrate = 0;
        quint32 duration = 0;
        quint32 sampleRate = 0;
        quint32 bitDepth = 0;
        quint8 others = 0;
        in >> hit.peer >> hit.path >> size >> hit.extension >> flags >> bitrate >> duration >> sampleRate >> bitDepth >>
            hit.queueLength >> hit.averageSpeed >> others;
        if (in.status() != QDataStream::Ok || others > kMaxOtherAttributes) {
            return std::nullopt;
        }
        for (quint8 other = 0; other < others; ++other) {
            quint32 code = 0;
            quint32 value = 0;
            in >> code >> value;
            hit.attributes.other.append({code, value});
        }
        hit.size = size;
        hit.freeSlot = (flags & FreeSlot) != 0;
        if ((flags & HasBitrate) != 0) {
            hit.attributes.bitrate = bitrate;
        }
        if ((flags & HasDuration) != 0) {
            hit.attributes.duration = duration;
        }
        if ((flags & HasVbr) != 0) {
            hit.attributes.vbr = (flags & Vbr) != 0;
        }
        if ((flags & HasSampleRate) != 0) {
            hit.attributes.sampleRate = sampleRate;
        }
        if ((flags & HasBitDepth) != 0) {
            hit.attributes.bitDepth = bitDepth;
        }
        hits.append(std::move(hit));
    }
    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }
    return hits;
}

std::optional<SearchCacheStore::Entry> SearchCacheStore::find(const QString &key)
{
    Statement query = m_db.prepare(u"SELECT query, searched_ms, peers, hits FROM search_cache WHERE query_key = ?1;");
    if (!query.isValid() || query.bind(1, key).step() != Statement::Step::Row) {
        return std::nullopt;
    }
    const qint64 searchedAt = query.int64(1);
    const qint64 age = m_now() - searchedAt;
    std::optional<QList<SearchHit>> hits;
    if (age >= 0 && age < m_lifetimeMs) {
        hits = decode(query.blob(3));
    }
    if (!hits) {
        // Expired, from the future (the clock went back), or damaged: forget it.
        Statement remove = m_db.prepare(u"DELETE FROM search_cache WHERE query_key = ?1;");
        if (!remove.isValid() || !remove.bind(1, key).run()) {
            qCWarning(lcSearchCache).noquote() << "couldn't forget a cached search:" << m_db.lastError();
        }
        return std::nullopt;
    }
    Entry entry;
    entry.outcome.query = query.text(0);
    entry.outcome.hits = std::move(*hits);
    entry.outcome.peers = static_cast<int>(query.int64(2));
    entry.outcome.fromCache = true;
    entry.outcome.searchedAtMs = searchedAt;
    return entry;
}

bool SearchCacheStore::store(const QString &key, const SearchOutcome &outcome)
{
    if (outcome.hits.isEmpty() || !outcome.error.isEmpty()) {
        return true;
    }
    Statement upsert = m_db.prepare(
        u"INSERT INTO search_cache (query_key, query, searched_ms, peers, files, hits) VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT (query_key) DO UPDATE SET query = excluded.query, searched_ms = excluded.searched_ms, "
        "peers = excluded.peers, files = excluded.files, hits = excluded.hits;");
    if (!upsert.isValid()) {
        return false;
    }
    const bool stored = upsert.bind(1, key)
                            .bind(2, outcome.query)
                            .bind(3, outcome.searchedAtMs > 0 ? outcome.searchedAtMs : m_now())
                            .bind(4, std::max(outcome.peers, 0))
                            .bind(5, static_cast<qint64>(outcome.hits.size()))
                            .bind(6, encode(outcome.hits))
                            .run();
    prune();
    return stored;
}

void SearchCacheStore::prune()
{
    const qint64 now = m_now();
    Statement expired = m_db.prepare(u"DELETE FROM search_cache WHERE searched_ms < ?1 OR searched_ms > ?2;");
    Statement oldest = m_db.prepare(u"DELETE FROM search_cache WHERE query_key NOT IN "
                                    "(SELECT query_key FROM search_cache ORDER BY searched_ms DESC LIMIT ?1);");
    if (!expired.isValid() || !expired.bind(1, now - m_lifetimeMs).bind(2, now + m_lifetimeMs).run() ||
        !oldest.isValid() || !oldest.bind(1, kMaxSearches).run()) {
        qCWarning(lcSearchCache).noquote() << "couldn't prune the search cache:" << m_db.lastError();
    }
}

int SearchCacheStore::count()
{
    Statement query = m_db.prepare(u"SELECT COUNT(*) FROM search_cache;");
    return query.isValid() && query.step() == Statement::Step::Row ? static_cast<int>(query.int64(0)) : 0;
}

qint64 SearchCacheStore::sizeBytes()
{
    Statement query = m_db.prepare(u"SELECT COALESCE(SUM(LENGTH(hits)), 0) FROM search_cache;");
    return query.isValid() && query.step() == Statement::Step::Row ? query.int64(0) : 0;
}

bool SearchCacheStore::clear()
{
    Statement remove = m_db.prepare(u"DELETE FROM search_cache;");
    return remove.isValid() && remove.run();
}

} // namespace selecta::core
