// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GetTracksService.h"

#include "collection/Scanner.h"
#include "core/MatchCorrections.h"
#include "core/SearchService.h"
#include "core/TransferService.h"
#include "match/Matcher.h"
#include "match/Normalizer.h"
#include "match/Tracklist.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcGetTracks, "selecta.core.gettracks")

// What a line is looked up as in the library: its release, else what was typed.
match::LocalTrackQuery ownershipQuery(const GetLine &line)
{
    match::LocalTrackQuery query;
    query.artist = line.release ? line.release->artist() : line.artist;
    query.title = line.release ? line.release->title : line.title;
    query.album = line.release ? line.release->album : QString();
    query.durationMs =
        line.release && line.release->durationMs ? *line.release->durationMs : line.durationHintMs.value_or(0);
    return query;
}

// The line's track as corrections know it (LIB-6).
TrackIdentity lineIdentity(const GetLine &line)
{
    const match::LocalTrackQuery query = ownershipQuery(line);
    return trackIdentity(query.artist, query.title, query.durationMs);
}

using ReleaseState = GetLine::ReleaseState;

// An upgrade line takes only files of a higher tier than its file's (P-010): drops the others, near misses included.
// Returns how many of the files that passed the preset went.
int keepBetterThan(RankedChoices &choices, QualityTier floor)
{
    const auto notBetter = [floor](const FileChoice &choice) { return qualityTierOf(choice) <= floor; };
    const auto dropped = choices.files.removeIf(notBetter);
    if (choices.nearMisses.removeIf(notBetter) > 0) {
        choices.belowQuality = static_cast<int>(choices.nearMisses.size());
    }
    return static_cast<int>(dropped);
}

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
    // Lines whose resolution a quit interrupted are still Pending, so they resolve again once a resolver is set;
    // searches a quit interrupted start over when the user finds again.
    m_lines = m_store.all();
    for (GetLine &line : m_lines) {
        if (line.findState == GetLine::FindState::Queued || line.findState == GetLine::FindState::Searching) {
            line.findState = GetLine::FindState::Idle;
        }
    }
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
        for (qsizetype i = m_lines.size() - added.size(); i < m_lines.size(); ++i) {
            if (checkOwnership(m_lines[i]) && !m_store.update(m_lines[i])) {
                qCWarning(lcGetTracks) << "couldn't save line" << m_lines[i].id << ':' << m_db.lastError();
            }
        }
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
        for (qsizetype i = m_lines.size() - added.size(); i < m_lines.size(); ++i) {
            if (checkOwnership(m_lines[i]) && !m_store.update(m_lines[i])) {
                qCWarning(lcGetTracks) << "couldn't save line" << m_lines[i].id << ':' << m_db.lastError();
            }
        }
        Q_EMIT linesReset(m_lines);
    }
    return result;
}

GetTracksService::UpgradeResult GetTracksService::addUpgrades(const QList<UpgradeRequest> &tracks)
{
    UpgradeResult result;
    QList<GetLine> candidates;
    for (const UpgradeRequest &track : tracks) {
        if (track.path.isEmpty()) {
            continue;
        }
        GetLine line;
        line.artist = track.artist.trimmed();
        line.title = track.title.trimmed();
        line.raw = line.artist.isEmpty() ? line.title : line.artist + u" - "_s + line.title;
        if (line.raw.isEmpty()) {
            line.raw = QFileInfo(track.path).completeBaseName(); // nothing tagged: what it's called
        }
        if (track.durationMs > 0) {
            line.durationHintMs = track.durationMs;
        }
        line.isrc = track.isrc.trimmed();
        line.upgradeOf = track.path;
        // One line per file: the same track twice in the collection is two upgrades, and an ordinary line for the track
        // doesn't stop one.
        line.dedupeKey = u"upgrade:"_s + collection::comparisonKey(track.path);
        line.addedAt = m_now();
        candidates.append(line);
    }
    const QList<GetLine> added = m_store.append(candidates);
    if (added.isEmpty() && !candidates.isEmpty() && m_store.count() == 0) {
        qCWarning(lcGetTracks) << "couldn't save the upgrade lines:" << m_db.lastError();
    }
    result.already = static_cast<int>(candidates.size() - added.size());
    if (added.isEmpty()) {
        return result;
    }
    m_lines.append(added);
    for (const GetLine &line : added) {
        result.lines.append(line.id);
    }
    Q_EMIT linesReset(m_lines);
    resolveNext();
    find(result.lines); // each searched once its release lookup is done
    return result;
}

void GetTracksService::remove(const QList<qint64> &ids)
{
    if (ids.isEmpty()) {
        return;
    }
    for (const qint64 id : ids) {
        if (const GetLine *line = lineWithId(id)) {
            cancelDownloadOf(*line);
        }
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
        if (const auto it = m_finding.find(id); it != m_finding.end()) {
            if (m_search) {
                m_search->cancel(*it);
            }
            m_finding.erase(it);
        }
        m_findAfterLookup.remove(id);
        if (const auto it = m_peerSearches.find(id); it != m_peerSearches.end()) {
            if (m_search) {
                m_search->cancel(*it);
            }
            m_peerSearches.erase(it);
        }
    }
    const QSet<qint64> gone(ids.cbegin(), ids.cend());
    m_lines.removeIf([&gone](const GetLine &line) { return gone.contains(line.id); });
    Q_EMIT linesReset(m_lines);
    resolveNext();
}

void GetTracksService::clear()
{
    for (const GetLine &line : std::as_const(m_lines)) {
        cancelDownloadOf(line);
    }
    if (!m_store.clear()) {
        qCWarning(lcGetTracks) << "couldn't clear the list:" << m_db.lastError();
        return;
    }
    for (auto &resolution : m_resolving) {
        resolution.cancel();
    }
    m_resolving.clear();
    if (m_search) {
        for (const quint64 search : std::as_const(m_finding)) {
            m_search->cancel(search);
        }
        for (const quint64 search : std::as_const(m_peerSearches)) {
            m_search->cancel(search);
        }
    }
    m_finding.clear();
    m_findAfterLookup.clear();
    m_peerSearches.clear();
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
        checkOwnership(*line); // the release may say it's owned
        store(*line);
        if (m_findAfterLookup.remove(id) && line->findState == GetLine::FindState::Queued) {
            startFinding(*line);
        }
    }
    resolveNext();
}

void GetTracksService::setSearch(SearchService *search, std::function<QStringList()> excludedPhrases)
{
    m_search = search;
    m_excludedPhrases = std::move(excludedPhrases);
}

void GetTracksService::setFilter(const SearchFilter &filter)
{
    if (filter == m_filter) {
        return;
    }
    m_filter = filter;
    // Lines already found are found again under the new filter: the search cache answers those it still has.
    QList<qint64> again;
    for (const GetLine &line : std::as_const(m_lines)) {
        const bool found = line.findState == GetLine::FindState::Ready || line.findState == GetLine::FindState::Check ||
                           line.findState == GetLine::FindState::NotFound;
        if (found && line.downloadState == GetLine::DownloadState::None) {
            again.append(line.id);
        }
    }
    if (!again.isEmpty()) {
        find(again);
    }
}

void GetTracksService::find(const QList<qint64> &ids)
{
    if (!m_search) {
        return;
    }
    const QSet<qint64> only(ids.cbegin(), ids.cend());
    for (GetLine &line : m_lines) {
        if (!only.isEmpty() && !only.contains(line.id)) {
            continue;
        }
        // Every line still to find; asked for by name, also lines found before or owned (to look for a better copy).
        const bool idle = line.findState == GetLine::FindState::Idle ||
                          (!only.isEmpty() && line.findState != GetLine::FindState::Queued &&
                           line.findState != GetLine::FindState::Searching);
        if (!idle || line.downloadState != GetLine::DownloadState::None || m_finding.contains(line.id)) {
            continue;
        }
        if (line.releaseState == ReleaseState::Pending && m_resolve) {
            // The release decides the query and the ranking: search once the lookup is done.
            m_findAfterLookup.insert(line.id);
            line.findState = GetLine::FindState::Queued;
            store(line);
            continue;
        }
        startFinding(line);
    }
}

void GetTracksService::startFinding(GetLine &line)
{
    QString why;
    const QString query = FileRanking::query(line, m_excludedPhrases ? m_excludedPhrases() : QStringList(), &why);
    if (query.isEmpty()) {
        line.findState = GetLine::FindState::NotFound;
        line.message = std::move(why);
        store(line);
        return;
    }
    const qint64 id = line.id;
    const QPointer<GetTracksService> self(this);
    const quint64 search = m_search->enqueue(
        query, governor::SearchSource::GetTracks,
        [self, id](const SearchOutcome &outcome) {
            if (self) {
                self->found(id, outcome);
            }
        },
        [self, id] {
            if (GetLine *started = self ? self->lineWithId(id) : nullptr) {
                started->findState = GetLine::FindState::Searching;
                self->store(*started);
            }
        });
    m_finding.insert(id, search);
    line.findState = GetLine::FindState::Queued;
    line.message.clear();
    store(line);
}

void GetTracksService::found(qint64 id, const SearchOutcome &outcome)
{
    m_finding.remove(id);
    GetLine *line = lineWithId(id);
    if (!line) {
        return;
    }
    line->foundThisSession = true;
    line->nearMisses.clear();
    if (!outcome.error.isEmpty()) {
        line->findState = GetLine::FindState::NotFound;
        line->message = outcome.error;
        line->best.reset();
        line->alternatives.clear();
        store(*line);
        return;
    }
    RankedChoices choices = FileRanking::rank(*line, outcome, m_filter, m_peerLoad);
    int notBetter = 0;
    QualityTier floor = QualityTier::Unknown;
    if (!line->upgradeOf.isEmpty()) {
        floor = m_tierOf ? m_tierOf(line->upgradeOf).value_or(QualityTier::Unknown) : QualityTier::Unknown;
        notBetter = keepBetterThan(choices, floor);
    }
    line->message.clear();
    if (choices.files.isEmpty()) {
        line->best.reset();
        line->alternatives.clear();
        line->ticked = false;
        line->findState = GetLine::FindState::NotFound;
        // The track's files the preset turned down, to get anyway; or files that aren't this track at all.
        line->nearMisses = choices.nearMisses;
        if (!choices.nearMisses.isEmpty()) {
            line->message =
                choices.belowQuality == 1
                    ? tr("1 file of this track found, below the quality wanted.")
                    : tr("%1 files of this track found, below the quality wanted.").arg(choices.belowQuality);
        } else if (notBetter > 0) {
            line->message = notBetter == 1
                                ? tr("1 file of this track found, none better than yours (%1).").arg(tierName(floor))
                                : tr("%1 files of this track found, none better than yours (%2).")
                                      .arg(notBetter)
                                      .arg(tierName(floor));
        } else if (choices.considered > 0) {
            line->message = choices.considered == 1
                                ? tr("1 file found, but not of this track.")
                                : tr("%1 files found, but none of this track.").arg(choices.considered);
        }
    } else {
        line->best = choices.files.constFirst();
        line->alternatives = choices.files.mid(1);
        const bool confident = line->best->fitsRelease;
        line->findState = confident ? GetLine::FindState::Ready : GetLine::FindState::Check;
        line->ticked = confident; // confident matches are pre-ticked (BATCH-5)
    }
    store(*line);
    // P-010: the user asked for a better copy of a track they have: a confident one comes by itself.
    if (!line->upgradeOf.isEmpty() && line->findState == GetLine::FindState::Ready &&
        line->downloadState == GetLine::DownloadState::None) {
        download({id});
    }
}

void GetTracksService::stopFinding()
{
    for (auto it = m_finding.cbegin(); it != m_finding.cend(); ++it) {
        if (m_search) {
            m_search->cancel(it.value());
        }
        if (GetLine *line = lineWithId(it.key())) {
            line->findState = GetLine::FindState::Idle;
            store(*line);
        }
    }
    m_finding.clear();
    for (const qint64 id : std::as_const(m_findAfterLookup)) {
        if (GetLine *line = lineWithId(id)) {
            line->findState = GetLine::FindState::Idle;
            store(*line);
        }
    }
    m_findAfterLookup.clear();
    const QList<qint64> lookingForPeers = m_peerSearches.keys();
    for (const qint64 id : lookingForPeers) {
        stopLookingForPeers(id);
    }
}

void GetTracksService::store(const GetLine &line)
{
    if (!m_store.update(line)) {
        qCWarning(lcGetTracks) << "couldn't save line" << line.id << ':' << m_db.lastError();
    }
    Q_EMIT lineChanged(line);
}

void GetTracksService::download(const QList<qint64> &ids)
{
    if (!m_transfers) {
        return;
    }
    const QList<qint64> order = [this, &ids] {
        QList<qint64> chosen;
        for (const GetLine &line : std::as_const(m_lines)) {
            if (ids.isEmpty() ? line.ticked : ids.contains(line.id)) {
                chosen.append(line.id);
            }
        }
        return chosen;
    }();
    using DownloadState = GetLine::DownloadState;
    // Spread the batch over the peers that have the same file (BATCH-7), counting what's already on its way.
    QHash<QString, int> load;
    for (const GetLine &line : std::as_const(m_lines)) {
        if (line.best &&
            (line.downloadState == DownloadState::Queued || line.downloadState == DownloadState::Transferring)) {
            ++load[line.best->peer];
        }
    }
    for (const qint64 id : order) {
        GetLine *line = lineWithId(id);
        if (line == nullptr || !line->best) {
            continue;
        }
        if (line->downloadState != DownloadState::None && line->downloadState != DownloadState::Failed) {
            continue; // on its way, or already in the Inbox
        }
        if (const qsizetype pick = spreadChoice(*line->best, line->alternatives, load); pick >= 0) {
            const FileChoice previous = *line->best;
            line->best = line->alternatives.takeAt(pick);
            line->alternatives.prepend(previous); // still the first to try if this one fails
        }
        ++load[line->best->peer];
        startDownload(*line);
    }
}

void GetTracksService::cancelDownloads(const QList<qint64> &ids)
{
    for (const qint64 id : ids) {
        if (const GetLine *line = lineWithId(id)) {
            cancelDownloadOf(*line);
        }
    }
}

void GetTracksService::startDownload(GetLine &line)
{
    if (!line.best) {
        return;
    }
    const FileChoice &best = *line.best;
    DownloadSpec spec;
    spec.peer = best.peer;
    spec.remotePath = best.remotePath;
    spec.size = best.size;
    spec.file = best;
    spec.release = line.release;
    spec.getLine = line.id;
    line.downloadId = 0;
    line.downloadState = GetLine::DownloadState::Queued;
    line.downloadProgress = -1.0;
    line.message.clear();
    const qint64 lineId = line.id;
    const std::optional<qint64> downloadId = m_transfers->enqueue(spec); // reports back through downloadChanged()
    GetLine *current = lineWithId(lineId);
    if (current == nullptr) {
        return;
    }
    if (!downloadId) {
        current->downloadState = GetLine::DownloadState::Failed;
        current->message = tr("Selecta couldn't start the download.");
    } else {
        current->downloadId = *downloadId;
    }
    store(*current);
}

void GetTracksService::cancelDownloadOf(const GetLine &line)
{
    using DownloadState = GetLine::DownloadState;
    if (m_transfers && line.downloadId != 0 &&
        (line.downloadState == DownloadState::Queued || line.downloadState == DownloadState::Transferring ||
         line.downloadState == DownloadState::Converting)) {
        m_transfers->cancel(line.downloadId);
    }
}

bool GetTracksService::tryAlternative(GetLine &line, const QString &reason)
{
    const QString failedPeer = line.best ? line.best->peer : QString();
    while (!line.alternatives.isEmpty()) {
        const FileChoice next = line.alternatives.takeFirst();
        if (next.peer == failedPeer) {
            continue; // the same peer just failed
        }
        line.best = next;
        startDownload(line);
        if (GetLine *current = lineWithId(line.id);
            current && current->downloadState != GetLine::DownloadState::Failed) {
            current->message = tr("%1; trying %2's copy instead").arg(reason, next.peer);
            store(*current);
        }
        return true;
    }
    return false;
}

void GetTracksService::downloadChanged(const Download &download)
{
    if (!download.getLine) {
        return;
    }
    GetLine *line = lineWithId(*download.getLine);
    if (line == nullptr || (line->downloadId != 0 && line->downloadId != download.id)) {
        return; // not this line's current download
    }
    line->downloadId = download.id;
    line->message = download.message;
    const bool atThePeer = download.state == Download::State::Queued;
    line->placeInQueue = atThePeer ? download.placeInQueue : std::nullopt;
    line->stuck = atThePeer && download.queue.isStuck();
    if (download.state != Download::State::Waiting && !atThePeer) {
        line->peersNote.clear(); // its turn came, or it ended: other peers are beside the point
    }
    using DownloadState = GetLine::DownloadState;
    switch (download.state) {
    case Download::State::Waiting:
    case Download::State::Queued:
        line->downloadState = DownloadState::Queued;
        line->downloadProgress = -1.0;
        break;
    case Download::State::Transferring:
        line->downloadState = DownloadState::Transferring;
        break;
    case Download::State::Converting:
        line->downloadState = DownloadState::Converting;
        line->downloadProgress = -1.0;
        break;
    case Download::State::Arrived:
        line->downloadState = DownloadState::Arrived;
        line->downloadProgress = -1.0;
        break;
    case Download::State::Failed:
        line->downloadProgress = -1.0;
        // A file that arrived whole but couldn't be converted or published isn't the peer's fault: no other copy.
        if ((download.size > 0 && download.received >= download.size) || !tryAlternative(*line, download.message)) {
            line = lineWithId(*download.getLine); // tryAlternative may have changed the list
            if (line != nullptr && line->downloadId == download.id) {
                line->downloadState = DownloadState::Failed;
                line->message = download.message;
            }
        } else {
            return; // stored by the new download
        }
        break;
    case Download::State::Cancelled:
        line->downloadState = DownloadState::None;
        line->downloadProgress = -1.0;
        line->downloadId = 0;
        break;
    }
    if (line != nullptr) {
        store(*line);
    }
}

void GetTracksService::downloadProgress(qint64 downloadId, qint64 bytesDone, qint64 bytesTotal)
{
    for (GetLine &line : m_lines) {
        if (line.downloadId != downloadId) {
            continue;
        }
        const double fraction =
            bytesTotal > 0 ? std::clamp(static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 0.0, 1.0)
                           : -1.0;
        if (std::abs(fraction - line.downloadProgress) >= 0.01 || (fraction >= 1.0 && line.downloadProgress < 1.0)) {
            line.downloadProgress = fraction;
            Q_EMIT lineChanged(line); // transient: not saved
        }
        return;
    }
}

void GetTracksService::downloadsRestored(const QList<Download> &unfinished)
{
    QSet<qint64> busyLines;
    for (const Download &download : unfinished) {
        if (download.getLine) {
            busyLines.insert(*download.getLine);
        }
    }
    using DownloadState = GetLine::DownloadState;
    for (GetLine &line : m_lines) {
        const bool inFlight = line.downloadState == DownloadState::Queued ||
                              line.downloadState == DownloadState::Transferring ||
                              line.downloadState == DownloadState::Converting;
        if (inFlight && !busyLines.contains(line.id)) {
            line.downloadState = DownloadState::None;
            line.downloadId = 0;
            line.message = tr("The download was interrupted; get it again.");
            store(line);
        }
    }
}

void GetTracksService::getAnyway(qint64 id, const QString &peer, const QByteArray &remotePath)
{
    GetLine *line = lineWithId(id);
    if (line == nullptr || m_transfers == nullptr || line->findState != GetLine::FindState::NotFound ||
        line->downloadState != GetLine::DownloadState::None) {
        return;
    }
    const auto chosen = std::ranges::find_if(line->nearMisses, [&](const FileChoice &choice) {
        return choice.peer == peer && choice.remotePath == remotePath;
    });
    if (chosen == line->nearMisses.end()) {
        return; // found again since: not one of its near misses any more
    }
    line->best = *chosen;
    line->nearMisses.erase(chosen);
    line->alternatives = std::exchange(line->nearMisses, {}); // the copies to try if this one fails
    line->findState = GetLine::FindState::Check;              // below the quality wanted: the user's call
    line->ticked = true;
    startDownload(*line);
}

void GetTracksService::switchPeer(qint64 id, const QString &peer, const QByteArray &remotePath)
{
    GetLine *line = lineWithId(id);
    // Only a download still waiting for its turn moves: one arriving keeps its bytes.
    if (line == nullptr || m_transfers == nullptr || !line->best ||
        line->downloadState != GetLine::DownloadState::Queued || m_peerSearches.contains(id)) {
        return;
    }
    const QString current = line->best->peer;
    qsizetype pick = -1;
    for (qsizetype i = 0; i < line->alternatives.size() && pick < 0; ++i) {
        const FileChoice &choice = line->alternatives.at(i);
        const bool another = choice.peer != current && !line->leftPeers.contains(choice.peer);
        if (another && (peer.isEmpty() || (choice.peer == peer && choice.remotePath == remotePath))) {
            pick = i;
        }
    }
    if (pick >= 0) {
        const FileChoice choice = line->alternatives.takeAt(pick);
        moveTo(*line, choice);
    } else if (peer.isEmpty()) {
        lookForOtherPeers(*line); // none known: find some
    }
}

void GetTracksService::moveTo(GetLine &line, const FileChoice &choice)
{
    if (!line.best) {
        return; // nothing waits at a peer: nothing to move
    }
    const GetLine before = line; // where it waits: what it falls back on
    const QString left = line.best->peer;
    const qint64 waitedWith = line.downloadId;
    const qint64 id = line.id;
    line.best = choice;
    line.findState =
        choice.shortfall.isEmpty() && choice.fitsRelease ? GetLine::FindState::Ready : GetLine::FindState::Check;
    line.placeInQueue.reset();
    line.stuck = false;
    line.peersNote.clear();
    // The other peer's file first, so the line never waits nowhere: its download's reports are the line's from here.
    startDownload(line);
    GetLine *current = lineWithId(id);
    if (current == nullptr) {
        return;
    }
    if (current->downloadId == 0) {
        // It couldn't be asked for: the line stays as it was, in the queue it waits in (its status, stuck or not,
        // included: the transfers report it again only when it changes), with the other copy still to try.
        *current = before;
        current->alternatives.prepend(choice);
        current->peersNote = tr("Selecta couldn't ask %1 for the file.").arg(choice.peer);
        store(*current);
        return;
    }
    if (!current->leftPeers.contains(left)) {
        current->leftPeers.append(left);
    }
    // Withdrawn from the peer's queue: there is no message for that, so its offer, if it comes, is answered
    // "Cancelled" and the peer moves on to its next user at once (references/network-etiquette-and-safe-development.md
    // §5.3). The governor counts the queue place given back. Its report isn't the line's current download's: the line
    // ignores it.
    if (waitedWith != 0 && waitedWith != current->downloadId) {
        m_transfers->cancel(waitedWith);
    }
}

void GetTracksService::lookForOtherPeers(GetLine &line)
{
    if (m_search == nullptr || m_peerSearches.contains(line.id)) {
        return;
    }
    QString why;
    const QString query = FileRanking::query(line, m_excludedPhrases ? m_excludedPhrases() : QStringList(), &why);
    if (query.isEmpty()) {
        line.peersNote = std::move(why);
        store(line);
        return;
    }
    const qint64 id = line.id;
    const QPointer<GetTracksService> self(this);
    // The user waits on this one: it goes ahead of the list's own searches, as a search typed by hand would. The
    // governor paces it all the same, and a recent identical search is answered from the cache.
    const quint64 search =
        m_search->enqueue(query, governor::SearchSource::Manual, [self, id](const SearchOutcome &outcome) {
            if (self) {
                self->otherPeersFound(id, outcome);
            }
        });
    m_peerSearches.insert(id, search);
    line.lookingForPeers = true;
    line.peersNote.clear();
    store(line);
}

void GetTracksService::otherPeersFound(qint64 id, const SearchOutcome &outcome)
{
    if (!m_peerSearches.remove(id)) {
        return; // stopped meanwhile
    }
    GetLine *line = lineWithId(id);
    if (line == nullptr) {
        return;
    }
    line->lookingForPeers = false;
    line->foundThisSession = true;
    if (!outcome.error.isEmpty()) {
        line->peersNote = tr("Selecta couldn't look for another peer: %1").arg(outcome.error);
        store(*line);
        return;
    }
    const RankedChoices choices = FileRanking::rank(*line, outcome, m_filter, m_peerLoad);
    // A file got anyway, below the quality wanted, may now have a copy that passes; else the other near misses.
    const bool below = line->best && !line->best->shortfall.isEmpty();
    const QList<FileChoice> &found = below && choices.files.isEmpty() ? choices.nearMisses : choices.files;
    QList<FileChoice> others;
    for (const FileChoice &choice : found) {
        if ((!line->best || choice.peer != line->best->peer) && !line->leftPeers.contains(choice.peer)) {
            others.append(choice);
        }
    }
    line->alternatives = others;
    if (others.isEmpty()) {
        line->peersNote = below ? tr("No other peer has it.") : tr("No other peer has it in the quality wanted.");
        store(*line);
        return;
    }
    if (line->downloadState != GetLine::DownloadState::Queued) {
        store(*line); // its turn came meanwhile: the other copies stay for later
        return;
    }
    const FileChoice next = line->alternatives.takeFirst();
    moveTo(*line, next);
}

void GetTracksService::stopLookingForPeers(qint64 id)
{
    if (const auto it = m_peerSearches.find(id); it != m_peerSearches.end()) {
        if (m_search) {
            m_search->cancel(*it);
        }
        m_peerSearches.erase(it);
    }
    if (GetLine *line = lineWithId(id); line != nullptr && line->lookingForPeers) {
        line->lookingForPeers = false;
        store(*line);
    }
}

void GetTracksService::setLibraryIndex(std::shared_ptr<const match::MatchIndex> index)
{
    m_index = std::move(index);
    for (GetLine &line : m_lines) {
        if (checkOwnership(line)) {
            store(line);
        }
    }
}

bool GetTracksService::checkOwnership(GetLine &line)
{
    using FindState = GetLine::FindState;
    // An upgrade is about a track the user has: never "You have it".
    if (!m_index || !line.upgradeOf.isEmpty() || line.downloadState != GetLine::DownloadState::None ||
        line.findState == FindState::Queued || line.findState == FindState::Searching) {
        return false;
    }
    if (const std::optional<OwnedCopy> copy = ownedCopy(line)) {
        const bool inCollection = copy->source == match::Source::Collection;
        const FindState state = inCollection ? FindState::Owned : FindState::InInbox;
        QString message;
        if (inCollection) {
            message = copy->sure ? tr("In your collection already") : tr("Probably in your collection already");
        } else {
            message = copy->sure ? tr("In your Inbox already") : tr("Probably in your Inbox already");
        }
        if (line.findState == state && line.message == message && !line.ticked) {
            return false;
        }
        line.findState = state;
        line.message = std::move(message);
        line.ticked = false;
        return true;
    }
    if (line.findState == FindState::Owned || line.findState == FindState::InInbox) {
        // The copy went away (deleted, filed elsewhere, a folder removed from Settings, or the user said it's another
        // track): find it again.
        line.findState = line.best ? FindState::Ready : FindState::Idle;
        line.message.clear();
        return true;
    }
    return false;
}

std::optional<GetTracksService::OwnedCopy> GetTracksService::ownedCopy(const GetLine &line)
{
    const match::LocalTrackQuery query = ownershipQuery(line);
    if (!m_index || query.title.isEmpty()) {
        return std::nullopt;
    }
    const QList<match::RecordMatch> matches = match::Matcher(m_index).matchLocal(query, 8);
    // LIB-6: the user's verdicts on this track and a copy (another version included, when they said it's the same).
    MatchCorrectionStore corrections(m_db);
    const bool corrected = !corrections.isEmpty();
    const TrackIdentity track = lineIdentity(line);
    const auto owned = [&](match::Source source) -> std::optional<OwnedCopy> {
        for (const match::RecordMatch &candidate : matches) {
            if (candidate.source != source || candidate.tier == match::Tier::None) {
                continue;
            }
            const std::optional<Verdict> verdict =
                corrected ? corrections.verdict(CorrectionKind::GetLine, track,
                                                libraryIdentity(m_db, candidate.id, candidate.source))
                          : std::nullopt;
            const bool same =
                candidate.relation == match::Relation::Same || candidate.relation == match::Relation::Upgrade;
            if (verdict == Verdict::Different || (!same && verdict != Verdict::Same)) {
                continue;
            }
            return OwnedCopy{candidate.id, candidate.source,
                             verdict == Verdict::Same || candidate.tier == match::Tier::Exact};
        }
        return std::nullopt;
    };
    if (std::optional<OwnedCopy> copy = owned(match::Source::Collection)) {
        return copy;
    }
    return owned(match::Source::Inbox);
}

void GetTracksService::notOwned(const QList<qint64> &ids)
{
    using FindState = GetLine::FindState;
    QList<qint64> toFind;
    for (const qint64 id : ids) {
        GetLine *line = lineWithId(id);
        if (!line || (line->findState != FindState::Owned && line->findState != FindState::InInbox)) {
            continue;
        }
        const std::optional<OwnedCopy> copy = ownedCopy(*line);
        if (!copy) {
            continue;
        }
        if (!MatchCorrectionStore(m_db).record(CorrectionKind::GetLine, lineIdentity(*line),
                                               libraryIdentity(m_db, copy->id, copy->source), Verdict::Different,
                                               m_now())) {
            qCWarning(lcGetTracks) << "couldn't remember the correction of line" << id << ':' << m_db.lastError();
            continue;
        }
        if (checkOwnership(*line)) {
            store(*line);
        }
        if (line->findState != FindState::Owned && line->findState != FindState::InInbox) {
            toFind.append(id);
        }
    }
    if (!toFind.isEmpty()) {
        find(toFind);
    }
}

void GetTracksService::recheckOwnership()
{
    for (GetLine &line : m_lines) {
        if (checkOwnership(line)) {
            store(line);
        }
    }
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
