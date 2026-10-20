// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/WishlistService.h"

#include "governor/QueryHygiene.h"
#include "match/RemotePathParser.h"
#include "storage/Database.h"

#include <QDataStream>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QSet>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcWishlist, "selecta.core.wishlist")

using storage::Statement;

constexpr auto kDefaultInterval = std::chrono::minutes(12);

quint64 fileKey(const SearchHit &hit)
{
    return qHash(hit.peer) ^ (qHash(hit.path) * 1'000'003ULL);
}

QByteArray encodeSeen(const QList<quint64> &seen)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << seen;
    return qCompress(bytes);
}

QList<quint64> decodeSeen(const QByteArray &blob)
{
    if (blob.isEmpty()) {
        return {};
    }
    const QByteArray bytes = qUncompress(blob);
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_5);
    QList<quint64> seen;
    stream >> seen;
    return stream.status() == QDataStream::Ok ? seen : QList<quint64>();
}

WishlistItem readItem(const Statement &row)
{
    return WishlistItem{.id = row.int64(0),
                        .text = row.text(1),
                        .createdAt = row.int64(2),
                        .lastRunMs = row.int64(3),
                        .found = static_cast<int>(row.int64(4))};
}

} // namespace

WishlistService::WishlistService(storage::Database &db, SearchService &search, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_search(search)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_timer(new QTimer(this))
{
    qRegisterMetaType<WishlistItem>();
    qRegisterMetaType<QList<WishlistItem>>();
    m_timer->setInterval(kDefaultInterval);
    connect(m_timer, &QTimer::timeout, this, &WishlistService::runNext);
    m_timer->start();
}

WishlistService::~WishlistService()
{
    if (m_running != 0) {
        m_search.cancel(m_running);
    }
}

std::optional<qint64> WishlistService::add(const QString &text)
{
    const QString cleaned = text.simplified();
    const QString key = governor::queryKey(governor::cleanQuery(cleaned, {}).query);
    if (key.isEmpty()) {
        return std::nullopt;
    }
    Statement existing = m_db.prepare(u"SELECT id FROM wishlist WHERE query_key = ?1;");
    if (existing.isValid() && existing.bind(1, key).step() == Statement::Step::Row) {
        return existing.int64(0);
    }
    Statement insert = m_db.prepare(u"INSERT INTO wishlist (text, query_key, created_at) VALUES (?1, ?2, ?3);");
    if (!insert.isValid() || !insert.bind(1, cleaned).bind(2, key).bind(3, m_now()).run()) {
        qCWarning(lcWishlist) << "couldn't save" << cleaned << ':' << m_db.lastError();
        return std::nullopt;
    }
    const qint64 id = m_db.lastInsertRowId();
    publish();
    return id;
}

void WishlistService::remove(qint64 id)
{
    Statement remove = m_db.prepare(u"DELETE FROM wishlist WHERE id = ?1;");
    if (!remove.isValid() || !remove.bind(1, id).run()) {
        qCWarning(lcWishlist) << "couldn't remove" << id << ':' << m_db.lastError();
    }
    publish();
}

void WishlistService::markSeen(qint64 id)
{
    Statement update = m_db.prepare(u"UPDATE wishlist SET found = 0 WHERE id = ?1;");
    if (!update.isValid() || !update.bind(1, id).run()) {
        qCWarning(lcWishlist) << "couldn't mark" << id << "seen:" << m_db.lastError();
    }
    publish();
}

void WishlistService::setInterval(std::chrono::milliseconds interval)
{
    m_timer->setInterval(std::max(interval, std::chrono::milliseconds(std::chrono::minutes(2))));
}

std::chrono::milliseconds WishlistService::interval() const
{
    return m_timer->intervalAsDuration();
}

void WishlistService::runNext()
{
    if (m_running != 0) {
        return; // one at a time: a background search is never urgent
    }
    // The search that waited longest (never run first).
    Statement next = m_db.prepare(u"SELECT id, text FROM wishlist ORDER BY last_run_ms, created_at, id LIMIT 1;");
    if (!next.isValid() || next.step() != Statement::Step::Row) {
        return;
    }
    const qint64 id = next.int64(0);
    const QString text = next.text(1);
    m_running = m_search.enqueue(
        // Fresh results, not the cache's: the point is what's new (the governor still keeps repeats quiet).
        SearchService::Request{.query = text, .source = governor::SearchSource::Wishlist, .refresh = true},
        [this, id](const SearchOutcome &outcome) { finished(id, outcome); });
}

void WishlistService::finished(qint64 id, const SearchOutcome &outcome)
{
    m_running = 0;
    Statement read = m_db.prepare(u"SELECT seen, found FROM wishlist WHERE id = ?1;");
    if (!read.isValid() || read.bind(1, id).step() != Statement::Step::Row) {
        return; // removed meanwhile
    }
    QList<quint64> seen = decodeSeen(read.blob(0));
    const int foundBefore = static_cast<int>(read.int64(1));
    QSet<quint64> known(seen.cbegin(), seen.cend());
    int fresh = 0;
    for (const SearchHit &hit : outcome.hits) {
        const QString extension =
            hit.extension.isEmpty() ? QFileInfo(QString::fromUtf8(hit.path)).suffix() : hit.extension;
        if (match::RemotePathParser::fileKind(extension.toLower()) != match::FileKind::Audio) {
            continue;
        }
        const quint64 key = fileKey(hit);
        if (!known.contains(key)) {
            known.insert(key);
            seen.append(key);
            ++fresh;
        }
    }
    if (seen.size() > kMaxSeen) {
        seen = seen.mid(seen.size() - kMaxSeen); // the oldest go first
    }
    if (!outcome.error.isEmpty()) {
        qCInfo(lcWishlist).noquote() << "wishlist search for" << id << "didn't run:" << outcome.error;
    }
    Statement update = m_db.prepare(u"UPDATE wishlist SET last_run_ms = ?1, found = ?2, seen = ?3 WHERE id = ?4;");
    if (!update.isValid() ||
        !update.bind(1, m_now()).bind(2, foundBefore + fresh).bind(3, encodeSeen(seen)).bind(4, id).run()) {
        qCWarning(lcWishlist) << "couldn't record the run of" << id << ':' << m_db.lastError();
    }
    publish();
    if (fresh > 0) {
        const QList<WishlistItem> all = items();
        const auto item = std::ranges::find(all, id, &WishlistItem::id);
        if (item != all.cend()) {
            Q_EMIT found(*item, fresh);
        }
    }
}

QList<WishlistItem> WishlistService::items()
{
    QList<WishlistItem> items;
    Statement query = m_db.prepare(
        u"SELECT id, text, created_at, last_run_ms, found FROM wishlist ORDER BY created_at DESC, id DESC;");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        items.append(readItem(query));
    }
    return items;
}

void WishlistService::publish()
{
    Q_EMIT itemsChanged(items());
}

} // namespace selecta::core
