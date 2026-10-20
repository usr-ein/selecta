// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ManualSearchService.h"

#include "match/QueryPlanner.h"
#include "match/Tracklist.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcManualSearch, "selecta.core.manualsearch")

constexpr qsizetype kMaxCandidates = 8;
// "Names the track" for free text: the typed words cover the release's artist and title this well (SRCH-17).
constexpr double kNamesTrackCoverage = 0.9;

// Soulseek's exclusions ("-live") stay in the network query but say nothing about the release.
QStringList exclusionsIn(const QString &text)
{
    QStringList words;
    const QStringList typed = text.split(u' ', Qt::SkipEmptyParts);
    for (const QString &word : typed) {
        if (word.size() > 1 && word.startsWith(u'-')) {
            words.append(word);
        }
    }
    return words;
}

QString withoutExclusions(const QString &text)
{
    QStringList words;
    const QStringList typed = text.split(u' ', Qt::SkipEmptyParts);
    for (const QString &word : typed) {
        if (word.size() <= 1 || !word.startsWith(u'-')) {
            words.append(word);
        }
    }
    return words.join(u' ');
}

// The release lookup for typed text: "Artist - Title" when it says so, otherwise the words as a title.
meta::ResolveQuery resolveQueryFor(const QString &text)
{
    meta::ResolveQuery query;
    const QString words = withoutExclusions(text);
    const QList<match::TracklistLine> lines = match::TracklistParser::parse(words);
    for (const match::TracklistLine &line : lines) {
        if (line.kind != match::TracklistLineKind::Track) {
            continue;
        }
        query.artist = line.artist;
        query.title = line.title;
        query.label = line.labelHint;
        if (line.durationSec > 0) {
            query.durationMs = qint64{line.durationSec} * 1000;
        }
        return query;
    }
    query.title = words;
    return query;
}

// The candidate the typed text names, or -1 (SRCH-17). With "Artist - Title" the resolver's confidence decides; free
// text is read the way a file name is, position-free and typo-tolerant (D28), and must cover the release's artist and
// title words.
int namedCandidate(const QString &text, const meta::Resolution &resolution, bool hasArtist)
{
    if (resolution.candidates.isEmpty()) {
        return -1;
    }
    if (hasArtist) {
        return resolution.confidence >= meta::Confidence::Likely ? 0 : -1;
    }
    // Read as the name of an audio file: the parser only looks inside names of tracks.
    const QString name = withoutExclusions(text) + u".mp3"_s;
    const match::ParsedRemote typed = match::RemotePathParser::parse(name);
    for (qsizetype i = 0; i < resolution.candidates.size() && i < kMaxCandidates; ++i) {
        const meta::Candidate &candidate = resolution.candidates.at(i);
        if (candidate.version == match::VersionCompatibility::OtherVersion) {
            continue;
        }
        const match::ReleaseFit fit = match::ReleaseScorer(referenceOf(candidate.release)).score(typed, 0);
        if (fit.titleCoverage >= kNamesTrackCoverage && fit.artistCoverage >= kNamesTrackCoverage &&
            fit.version != match::VersionCompatibility::OtherVersion) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

ManualSearchService::ManualSearchService(SearchService &search, QObject *parent)
    : QObject(parent)
    , m_search(search)
{
    m_pool.setMaxThreadCount(1); // batches take turns: analyses of one search stay in order
    m_pool.setObjectName(u"selecta-search"_s);
}

ManualSearchService::~ManualSearchService()
{
    for (auto &[id, session] : m_sessions) {
        if (session.networkSearch != 0) {
            m_search.cancel(session.networkSearch);
        }
        session.lookup.cancel();
    }
    m_pool.waitForDone(); // analyses still in the mailbox are dropped with it
}

ManualSearchService::Session *ManualSearchService::find(qint64 id)
{
    const auto found = m_sessions.find(id);
    return found == m_sessions.end() ? nullptr : &found->second;
}

const ManualSearchService::Session *ManualSearchService::find(qint64 id) const
{
    const auto found = m_sessions.find(id);
    return found == m_sessions.end() ? nullptr : &found->second;
}

qint64 ManualSearchService::open(const QString &text)
{
    const QString typed = text.simplified();
    const QString key = typed.toCaseFolded();
    if (withoutExclusions(typed).isEmpty()) {
        return 0;
    }
    for (const auto &[id, session] : m_sessions) {
        if (session.key == key) {
            return id;
        }
    }
    while (m_sessions.size() >= static_cast<std::size_t>(kMaxOpen)) {
        close(m_sessions.begin()->first);
    }
    const qint64 id = m_nextId++;
    Session &session = m_sessions[id];
    session.search.id = id;
    session.search.text = typed;
    session.key = key;
    session.lookupWait = std::make_unique<QTimer>();
    session.lookupWait->setSingleShot(true);
    session.lookupWait->setInterval(m_lookupWait);
    connect(session.lookupWait.get(), &QTimer::timeout, this, [this, id] {
        Session *waiting = find(id);
        if (waiting && waiting->search.state == ManualSearch::State::LookingUp) {
            qCDebug(lcManualSearch) << "release lookup slow: searching" << waiting->search.text << "as typed";
            send(*waiting, false); // the release still ranks the results when it comes
        }
    });

    if (!m_resolve) {
        send(session, false);
        return id;
    }
    const meta::ResolveQuery query = resolveQueryFor(typed);
    session.lookup = m_resolve(query);
    session.lookup.then(this,
                        [self = QPointer<ManualSearchService>(this), id](const meta::Result<meta::Resolution> &result) {
                            if (self) {
                                self->lookedUp(id, result);
                            }
                        });
    // A lookup that failed at once has already run its continuation (which sent the search).
    if (session.search.state == ManualSearch::State::LookingUp) {
        session.lookupWait->start();
        publish(session);
    }
    return id;
}

void ManualSearchService::lookedUp(qint64 id, const meta::Result<meta::Resolution> &result)
{
    Session *session = find(id);
    if (!session) {
        return;
    }
    session->lookupWait->stop();
    int named = -1;
    if (result.ok()) {
        const meta::Resolution &resolution = result.value();
        session->search.candidates = resolution.candidates.mid(0, kMaxCandidates);
        const meta::ResolveQuery query = resolveQueryFor(session->search.text);
        named = namedCandidate(session->search.text, resolution, !query.artist.isEmpty());
    } else {
        qCDebug(lcManualSearch).noquote() << "release lookup failed:" << result.error().message;
    }
    if (session->search.state == ManualSearch::State::LookingUp) {
        choose(*session, named, named >= 0);
        send(*session, false);
        return;
    }
    // The search went ahead as typed; the release now ranks what came back.
    if (named >= 0) {
        choose(*session, named, true);
        reanalyse(*session);
    }
    publish(*session);
}

void ManualSearchService::choose(Session &session, int candidate, bool automatically)
{
    ManualSearch &search = session.search;
    search.chosen = candidate >= 0 && candidate < search.candidates.size() ? candidate : -1;
    search.chosenAutomatically = search.chosen >= 0 && automatically;
    session.widerStep = 0;
    search.canWiden = false;
    if (const meta::Release *release = search.chosenRelease()) {
        const QList<match::QueryPlan> ladder =
            match::QueryPlanner(excludedPhrases()).widerLadder(referenceOf(*release));
        search.canWiden = std::ranges::any_of(ladder, &match::QueryPlan::isReady);
    }
}

void ManualSearchService::chooseRelease(qint64 id, int candidate)
{
    Session *session = find(id);
    if (!session || session->search.state == ManualSearch::State::LookingUp) {
        return;
    }
    choose(*session, candidate, false);
    reanalyse(*session);
    publish(*session);
}

void ManualSearchService::refresh(qint64 id)
{
    if (Session *session = find(id)) {
        session->lookupWait->stop();
        send(*session, true);
    }
}

void ManualSearchService::widen(qint64 id)
{
    Session *session = find(id);
    const meta::Release *release = session ? session->search.chosenRelease() : nullptr;
    if (!release) {
        return;
    }
    const QList<match::QueryPlan> ladder = match::QueryPlanner(excludedPhrases()).widerLadder(referenceOf(*release));
    for (qsizetype step = session->widerStep; step < ladder.size(); ++step) {
        if (ladder.at(step).isReady()) {
            session->widerStep = static_cast<int>(step) + 1;
            session->search.canWiden = std::any_of(ladder.cbegin() + step + 1, ladder.cend(),
                                                   [](const match::QueryPlan &plan) { return plan.isReady(); });
            send(*session, true);
            return;
        }
    }
    session->search.canWiden = false;
    publish(*session);
}

void ManualSearchService::close(qint64 id)
{
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end()) {
        return;
    }
    if (found->second.networkSearch != 0) {
        m_search.cancel(found->second.networkSearch);
    }
    found->second.lookup.cancel();
    m_sessions.erase(found);
    Q_EMIT searchClosed(id);
}

QStringList ManualSearchService::excludedPhrases() const
{
    return m_excludedPhrases ? m_excludedPhrases() : QStringList();
}

std::optional<match::ReleaseReference> ManualSearchService::reference(const Session &session) const
{
    if (const meta::Release *release = session.search.chosenRelease()) {
        return referenceOf(*release);
    }
    return std::nullopt;
}

void ManualSearchService::send(Session &session, bool refresh)
{
    ManualSearch &search = session.search;
    if (session.networkSearch != 0) {
        m_search.cancel(session.networkSearch);
        session.networkSearch = 0;
    }
    // The query: spelled as the release when there is one (SRCH-19), the next wider step when asked; as typed
    // otherwise. The user's exclusions always go along.
    QString query = search.text;
    search.respelled = false;
    if (const meta::Release *release = search.chosenRelease()) {
        const match::QueryPlanner planner(excludedPhrases());
        const match::ReleaseReference reference = referenceOf(*release);
        match::QueryPlan plan = planner.plan(reference, match::QueryStep::Default, withoutExclusions(search.text));
        if (session.widerStep > 0) {
            const QList<match::QueryPlan> ladder = planner.widerLadder(reference);
            if (session.widerStep - 1 < ladder.size()) {
                plan = ladder.at(session.widerStep - 1);
            }
        }
        if (plan.status == match::QueryStatus::Blocked) {
            search.state = ManualSearch::State::Failed;
            search.message = tr("The server excludes “%1”, so this can't be searched for.").arg(plan.blockedPhrase);
            publish(session);
            return;
        }
        if (plan.isReady()) {
            const QStringList exclusions = exclusionsIn(search.text);
            query = exclusions.isEmpty() ? plan.text : plan.text + u' ' + exclusions.join(u' ');
            search.respelled = plan.respelled;
        }
    }

    // What's on screen stays until the new answers come: a refused or empty search again loses nothing.
    ++session.sendId;
    session.replacing = true;
    search.query = query;
    search.state = ManualSearch::State::Waiting;
    search.message.clear();

    const qint64 id = search.id;
    const quint64 sent = session.sendId;
    const QPointer<ManualSearchService> self(this);
    session.networkSearch = m_search.enqueue(
        SearchService::Request{.query = query, .source = governor::SearchSource::Manual, .refresh = refresh},
        [self, id, sent](const SearchOutcome &outcome) {
            if (self) {
                self->finished(id, sent, outcome);
            }
        },
        [self, id, sent] {
            Session *started = self ? self->find(id) : nullptr;
            if (started && started->sendId == sent && started->search.state == ManualSearch::State::Waiting) {
                started->search.state = ManualSearch::State::Searching;
                self->publish(*started);
            }
        },
        [self, id, sent](const QList<SearchHit> &added, int peers) {
            if (self) {
                self->progress(id, sent, added, peers);
            }
        });
    publish(session);
}

void ManualSearchService::startReplacing(Session &session)
{
    if (!std::exchange(session.replacing, false)) {
        return;
    }
    // The previous answers go; the first batch of the new ones replaces them on screen.
    ++session.generation;
    session.results.clear();
    session.hits.clear();
    session.analysedUpTo = 0;
    session.analysing = false;
    session.resetPending = true;
    session.search.files = 0;
    session.search.peers = 0;
    session.search.fromCache = false;
    session.search.searchedAtMs = 0;
}

void ManualSearchService::progress(qint64 id, quint64 sendId, const QList<SearchHit> &added, int peers)
{
    Session *session = find(id);
    if (!session || session->sendId != sendId || session->search.state == ManualSearch::State::Done) {
        return;
    }
    startReplacing(*session);
    session->hits.append(added);
    session->search.peers = peers;
    session->search.state = ManualSearch::State::Searching;
    analyseNext(*session);
}

void ManualSearchService::finished(qint64 id, quint64 sendId, const SearchOutcome &outcome)
{
    Session *session = find(id);
    if (!session || session->sendId != sendId) {
        return;
    }
    ManualSearch &search = session->search;
    session->networkSearch = 0;
    if (!outcome.error.isEmpty() && outcome.hits.isEmpty()) {
        session->replacing = false;
        // Answers already on screen stay, with the reason nothing new came.
        search.state = session->hits.isEmpty() ? ManualSearch::State::Failed : ManualSearch::State::Done;
        search.message = outcome.error;
        publish(*session);
        return;
    }
    const bool wasReplacing = session->replacing;
    startReplacing(*session);
    search.state = ManualSearch::State::Done;
    search.fromCache = outcome.fromCache;
    search.searchedAtMs = outcome.searchedAtMs;
    search.peers = outcome.peers;
    if (!outcome.query.isEmpty()) {
        search.query = outcome.query;
    }
    // `done` has every file; the ones progress didn't bring (all of them for cached results) follow.
    if (session->hits.size() < outcome.hits.size()) {
        session->hits.append(outcome.hits.mid(session->hits.size()));
    }
    if (wasReplacing && session->hits.isEmpty()) {
        session->resetPending = false;
        Q_EMIT resultsReset(search.id, {}); // nobody answered this time
    }
    analyseNext(*session);
    publish(*session);
}

void ManualSearchService::analyseNext(Session &session)
{
    if (session.analysing || session.analysedUpTo >= session.hits.size()) {
        return;
    }
    const qsizetype first = session.analysedUpTo;
    const bool reset = std::exchange(session.resetPending, false);
    session.analysedUpTo = session.hits.size();
    session.analysing = true;
    m_pool.start([this, id = session.search.id, generation = session.generation, reset, hits = session.hits.mid(first),
                  first = static_cast<int>(first), index = m_index, release = reference(session)] {
        post(Analysis{
            .id = id, .generation = generation, .reset = reset, .results = analyzeHits(hits, first, index, release)});
    });
}

void ManualSearchService::reanalyse(Session &session)
{
    // Every file received so far, analysed again; a batch still with the worker is dropped when it lands.
    ++session.generation;
    session.analysing = false;
    session.analysedUpTo = 0;
    session.resetPending = true;
    analyseNext(session);
}

void ManualSearchService::post(Analysis analysis)
{
    {
        const std::scoped_lock lock(m_mutex);
        m_analyses.append(std::move(analysis));
    }
    // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
    QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
}

void ManualSearchService::drain()
{
    QList<Analysis> analyses;
    {
        const std::scoped_lock lock(m_mutex);
        analyses.swap(m_analyses);
    }
    for (Analysis &analysis : analyses) {
        Session *session = find(analysis.id);
        if (!session || analysis.generation != session->generation) {
            continue; // closed, or overtaken by a new release, snapshot or network search
        }
        session->analysing = false;
        if (m_annotate) {
            m_annotate(analysis.results);
        }
        if (analysis.reset) {
            session->results = analysis.results;
            Q_EMIT resultsReset(session->search.id, session->results);
        } else {
            session->results.append(analysis.results);
            Q_EMIT resultsAdded(session->search.id, analysis.results);
        }
        session->search.files = static_cast<int>(session->results.size());
        analyseNext(*session);
        publish(*session);
    }
}

void ManualSearchService::setLibraryIndex(std::shared_ptr<const match::MatchIndex> index)
{
    m_index = std::move(index);
    for (auto &[id, session] : m_sessions) {
        if (!session.hits.isEmpty()) {
            reanalyse(session);
        }
    }
}

void ManualSearchService::refreshAnnotations()
{
    if (!m_annotate) {
        return;
    }
    for (auto &[id, session] : m_sessions) {
        if (session.results.isEmpty()) {
            continue;
        }
        m_annotate(session.results);
        Q_EMIT resultsChanged(id, session.results);
    }
}

int ManualSearchService::download(qint64 id, const QList<int> &indexes)
{
    const Session *session = find(id);
    if (!session || !m_enqueue) {
        return 0;
    }
    int accepted = 0;
    const meta::Release *chosen = session->search.chosenRelease();
    for (const int index : indexes) {
        if (index < 0 || index >= session->results.size()) {
            continue;
        }
        const SearchResult &result = session->results.at(index);
        if (!result.isAudio()) {
            continue;
        }
        ++accepted;
        if (chosen && result.fitsRelease()) {
            startDownload(result, *chosen);
            continue;
        }
        if (!m_resolve || (result.parsed.title.value.isEmpty() && result.parsed.artist.value.isEmpty())) {
            startDownload(result, std::nullopt);
            continue;
        }
        // Not the chosen release: look this file's own track up, so it's tagged from its release too (D29).
        meta::ResolveQuery query;
        query.artist = result.parsed.artist.value;
        query.title = result.parsed.title.value;
        if (result.durationSec() > 0) {
            query.durationMs = qint64{result.durationSec()} * 1000;
        }
        m_resolve(query).then(this, [self = QPointer<ManualSearchService>(this),
                                     result](const meta::Result<meta::Resolution> &resolution) {
            if (!self) {
                return;
            }
            std::optional<meta::Release> release;
            if (resolution.ok() && !resolution.value().candidates.isEmpty() &&
                resolution.value().confidence >= meta::Confidence::Likely) {
                release = resolution.value().candidates.constFirst().release;
            }
            self->startDownload(result, release);
        });
    }
    return accepted;
}

void ManualSearchService::startDownload(const SearchResult &result, const std::optional<meta::Release> &release)
{
    if (!m_enqueue) {
        return;
    }
    DownloadSpec spec;
    spec.peer = result.hit.peer;
    spec.remotePath = result.hit.path;
    spec.size = static_cast<qint64>(result.hit.size);
    spec.file = fileChoiceOf(result);
    spec.release = release;
    if (!m_enqueue(spec)) {
        qCWarning(lcManualSearch) << "couldn't queue" << result.path << "from" << result.hit.peer;
        return;
    }
    refreshAnnotations();
}

QList<ManualSearch> ManualSearchService::searches() const
{
    QList<ManualSearch> all;
    all.reserve(static_cast<qsizetype>(m_sessions.size()));
    for (const auto &[id, session] : m_sessions) {
        all.append(session.search);
    }
    return all;
}

std::optional<ManualSearch> ManualSearchService::search(qint64 id) const
{
    const Session *session = find(id);
    return session ? std::optional<ManualSearch>(session->search) : std::nullopt;
}

QList<SearchResult> ManualSearchService::results(qint64 id) const
{
    const Session *session = find(id);
    return session ? session->results : QList<SearchResult>();
}

std::optional<SearchResult> ManualSearchService::result(qint64 id, int index) const
{
    const Session *session = find(id);
    if (!session || index < 0 || index >= session->results.size()) {
        return std::nullopt;
    }
    return session->results.at(index);
}

void ManualSearchService::publish(const Session &session)
{
    Q_EMIT searchChanged(session.search);
}

void ManualSearchService::waitForIdle()
{
    for (int round = 0; round < 100; ++round) {
        m_pool.waitForDone();
        bool pending = false;
        {
            const std::scoped_lock lock(m_mutex);
            pending = !m_analyses.isEmpty();
        }
        if (!pending) {
            return;
        }
        drain();
    }
}

} // namespace selecta::core
