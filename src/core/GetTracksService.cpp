// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GetTracksService.h"

#include "match/Normalizer.h"
#include "match/Tracklist.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcGetTracks, "selecta.core.gettracks")

using ReleaseState = GetLine::ReleaseState;

// The key that makes "the same track twice" one line. Free text without artist falls back to its normalised words.
QString dedupeKeyFor(const QString &artist, const QString &title, const QString &raw)
{
    const match::TrackKeys keys = match::Normalizer::trackKeys(artist, title);
    if (!keys.exactKey.isEmpty()) {
        return keys.exactKey;
    }
    return raw.simplified().toCaseFolded();
}

// Stored with the line; mirrors meta's tier names.
QString confidenceText(meta::Confidence confidence)
{
    switch (confidence) {
    case meta::Confidence::Exact:
        return u"exact"_s;
    case meta::Confidence::Confident:
        return u"confident"_s;
    case meta::Confidence::Likely:
        return u"likely"_s;
    case meta::Confidence::Ambiguous:
        return u"ambiguous"_s;
    case meta::Confidence::None:
        break;
    }
    return u"none"_s;
}

ReleaseState stateFor(meta::Confidence confidence)
{
    switch (confidence) {
    case meta::Confidence::Exact:
    case meta::Confidence::Confident:
    case meta::Confidence::Likely:
        return ReleaseState::Resolved;
    case meta::Confidence::Ambiguous:
        return ReleaseState::Ambiguous;
    case meta::Confidence::None:
        break;
    }
    return ReleaseState::None;
}

} // namespace

GetTracksService::GetTracksService(storage::Database &db, std::function<qint64()> wallClock, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_now(wallClock ? std::move(wallClock) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_store(db)
{
    qRegisterMetaType<GetLine>();
    qRegisterMetaType<QList<GetLine>>();
    load();
}

GetTracksService::~GetTracksService()
{
    for (auto &resolution : m_resolving) {
        resolution.cancel();
    }
}

void GetTracksService::load()
{
    // Lines whose resolution a quit interrupted are still Pending, so they resolve again once a resolver is set.
    m_lines = m_store.all();
}

void GetTracksService::setResolver(ResolveFunction resolve)
{
    m_resolve = std::move(resolve);
    resolveNext();
}

GetTracksService::AddResult GetTracksService::add(const QString &text)
{
    AddResult result;
    QList<GetLine> candidates;
    const QList<match::TracklistLine> parsed = match::TracklistParser::parse(text);
    for (const match::TracklistLine &entry : parsed) {
        if (entry.kind == match::TracklistLineKind::Unidentified) {
            ++result.unidentified;
            continue;
        }
        if (entry.kind != match::TracklistLineKind::Track) {
            continue;
        }
        GetLine line;
        line.raw = entry.raw.trimmed();
        line.artist = entry.artist;
        line.title = entry.title;
        line.labelHint = entry.labelHint;
        if (entry.durationSec > 0) {
            line.durationHintMs = qint64{entry.durationSec} * 1000;
        }
        line.dedupeKey =
            entry.keys.exactKey.isEmpty() ? dedupeKeyFor(line.artist, line.title, line.raw) : entry.keys.exactKey;
        line.addedAt = m_now();
        candidates.append(line);
    }
    const QList<GetLine> added = m_store.append(candidates);
    if (added.isEmpty() && !candidates.isEmpty() && m_store.count() == 0) {
        qCWarning(lcGetTracks) << "couldn't save the new lines:" << m_db.lastError();
    }
    result.added = static_cast<int>(added.size());
    result.duplicates = static_cast<int>(candidates.size() - added.size());
    if (!added.isEmpty()) {
        m_lines.append(added);
        Q_EMIT linesReset(m_lines);
        resolveNext();
    }
    return result;
}

GetTracksService::AddResult GetTracksService::addReleases(const QList<meta::Release> &tracks)
{
    AddResult result;
    QList<GetLine> candidates;
    for (const meta::Release &release : tracks) {
        GetLine line;
        line.artist = release.artist();
        line.title = release.title;
        line.raw = line.artist.isEmpty() ? line.title : line.artist + u" - "_s + line.title;
        if (line.raw.trimmed().isEmpty()) {
            continue;
        }
        line.durationHintMs = release.durationMs;
        line.isrc = release.isrc;
        line.release = release;
        line.releaseState = ReleaseState::Resolved;
        line.releaseConfidence = confidenceText(meta::Confidence::Exact);
        line.dedupeKey = dedupeKeyFor(line.artist, line.title, line.raw);
        line.addedAt = m_now();
        candidates.append(line);
    }
    const QList<GetLine> added = m_store.append(candidates);
    result.added = static_cast<int>(added.size());
    result.duplicates = static_cast<int>(candidates.size() - added.size());
    if (!added.isEmpty()) {
        m_lines.append(added);
        Q_EMIT linesReset(m_lines);
    }
    return result;
}

void GetTracksService::remove(const QList<qint64> &ids)
{
    if (ids.isEmpty()) {
        return;
    }
    if (!m_store.remove(ids)) {
        qCWarning(lcGetTracks) << "couldn't remove lines:" << m_db.lastError();
        return;
    }
    for (const qint64 id : ids) {
        if (const auto it = m_resolving.find(id); it != m_resolving.end()) {
            it->cancel();
            m_resolving.erase(it);
        }
    }
    const QSet<qint64> gone(ids.cbegin(), ids.cend());
    m_lines.removeIf([&gone](const GetLine &line) { return gone.contains(line.id); });
    Q_EMIT linesReset(m_lines);
    resolveNext();
}

void GetTracksService::clear()
{
    if (!m_store.clear()) {
        qCWarning(lcGetTracks) << "couldn't clear the list:" << m_db.lastError();
        return;
    }
    for (auto &resolution : m_resolving) {
        resolution.cancel();
    }
    m_resolving.clear();
    m_lines.clear();
    Q_EMIT linesReset(m_lines);
}

void GetTracksService::setTicked(const QList<qint64> &ids, bool ticked)
{
    for (const qint64 id : ids) {
        GetLine *line = lineWithId(id);
        if (!line || line->ticked == ticked) {
            continue;
        }
        // Only lines with a file to get can be ticked (BATCH-6).
        if (ticked && !line->best) {
            continue;
        }
        line->ticked = ticked;
        store(*line);
    }
}

void GetTracksService::retry(const QList<qint64> &ids)
{
    for (const qint64 id : ids) {
        GetLine *line = lineWithId(id);
        if (!line || m_resolving.contains(id)) {
            continue;
        }
        line->releaseState = ReleaseState::Pending;
        line->message.clear();
        store(*line);
    }
    resolveNext();
}

void GetTracksService::chooseRelease(qint64 id, const meta::Release &release)
{
    GetLine *line = lineWithId(id);
    if (!line) {
        return;
    }
    if (const auto it = m_resolving.find(id); it != m_resolving.end()) {
        it->cancel();
        m_resolving.erase(it);
    }
    line->resolving = false;
    line->release = release;
    line->releaseState = ReleaseState::Resolved;
    line->releaseConfidence = u"chosen"_s;
    line->message.clear();
    store(*line);
    resolveNext();
}

void GetTracksService::resolveNext()
{
    if (!m_resolve) {
        return;
    }
    for (const GetLine &line : std::as_const(m_lines)) {
        if (m_resolving.size() >= kMaxResolving) {
            return;
        }
        if (line.releaseState != ReleaseState::Pending || m_resolving.contains(line.id)) {
            continue;
        }
        meta::ResolveQuery query;
        query.artist = line.artist;
        query.title = line.title;
        query.label = line.labelHint;
        query.durationMs = line.durationHintMs;
        query.isrc = line.isrc;
        const qint64 id = line.id;
        QFuture<meta::Result<meta::Resolution>> future = m_resolve(query);
        m_resolving.insert(id, future);
        if (GetLine *started = lineWithId(id)) {
            started->resolving = true;
            Q_EMIT lineChanged(*started);
        }
        // Runs on this thread. A cancelled resolution skips it; the QPointer guards against the service being gone.
        future.then(this, [self = QPointer<GetTracksService>(this), id](const meta::Result<meta::Resolution> &r) {
            if (self) {
                self->resolved(id, r);
            }
        });
    }
}

void GetTracksService::resolved(qint64 id, const meta::Result<meta::Resolution> &result)
{
    m_resolving.remove(id);
    GetLine *line = lineWithId(id);
    if (line) {
        line->resolving = false;
    }
    if (line && line->releaseState == ReleaseState::Pending) {
        if (!result.ok()) {
            line->releaseState = ReleaseState::Failed;
            line->message = result.error().message;
        } else if (result.value().candidates.isEmpty()) {
            line->releaseState = ReleaseState::None;
            line->release.reset();
            line->message.clear();
        } else {
            const meta::Resolution &resolution = result.value();
            line->releaseState = stateFor(resolution.confidence);
            line->release = resolution.candidates.first().release;
            line->releaseConfidence = confidenceText(resolution.confidence);
            if (line->releaseState == ReleaseState::None) {
                line->release.reset(); // below "Ambiguous": not worth showing as the release
            }
            line->message.clear();
        }
        store(*line);
    }
    resolveNext();
}

void GetTracksService::store(const GetLine &line)
{
    if (!m_store.update(line)) {
        qCWarning(lcGetTracks) << "couldn't save line" << line.id << ':' << m_db.lastError();
    }
    Q_EMIT lineChanged(line);
}

GetLine *GetTracksService::lineWithId(qint64 id)
{
    for (GetLine &line : m_lines) {
        if (line.id == id) {
            return &line;
        }
    }
    return nullptr;
}

} // namespace selecta::core
