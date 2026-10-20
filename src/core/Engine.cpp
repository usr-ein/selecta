// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Engine.h"

#include "base/Clock.h"
#include "core/GovernorStore.h"
#include "governor/Limits.h"
#include "meta/SpotifyLinks.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcEngine, "selecta.core.engine")

constexpr auto kActivityPruneInterval = 6h;

} // namespace

Engine::Engine(EngineOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    if (!m_options.wallClock) {
        m_options.wallClock = [] { return QDateTime::currentMSecsSinceEpoch(); };
    }
    if (m_options.clock) {
        m_clock = m_options.clock;
    } else {
        m_ownClock = std::make_unique<base::SteadyClock>();
        m_clock = m_ownClock.get();
    }
}

Engine::~Engine()
{
    stop();
}

bool Engine::fail(const QString &error)
{
    m_error = error;
    qCCritical(lcEngine).noquote() << "engine can't start:" << error;
    stop();
    Q_EMIT startFinished(false, error);
    return false;
}

bool Engine::start()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_started) {
        return true;
    }
    m_error.clear();

    m_db = std::make_unique<storage::Database>();
    if (!m_db->open(m_options.databasePath)) {
        return fail(tr("Can't open the library: %1").arg(m_db->lastError()));
    }
    // The UI thread migrates first at launch; this is a no-op then, and makes the engine usable on its own.
    const auto migration = storage::migrate(*m_db, storage::selectaMigrations());
    if (!migration.ok()) {
        return fail(tr("Can't update the library: %1").arg(migration.error));
    }

    m_governor = std::make_unique<governor::Governor>(*m_clock, m_options.governor);
    m_activity = std::make_unique<ActivityLog>(*m_db, m_options.wallClock);
    connect(m_governor.get(), &governor::Governor::decided, m_activity.get(), &ActivityLog::recordDecision);
    connect(m_activity.get(), &ActivityLog::recorded, this, &Engine::activityRecorded);
    connect(m_governor.get(), &governor::Governor::pausedChanged, this, &Engine::pausedChanged);
    connect(m_governor.get(), &governor::Governor::searchBudgetChanged, this, &Engine::reportSearchBudget);

    m_governorStore = std::make_unique<GovernorStore>(*m_db, *m_governor, m_options.wallClock);
    if (!m_governorStore->restore()) {
        // Not fatal: the limits still apply from now on, only earlier totals are lost.
        qCWarning(lcEngine) << "couldn't restore the governor's saved state:" << m_db->lastError();
    }

    MetaServices::Options metaOptions;
    metaOptions.tokens = m_options.tokenStore ? m_options.tokenStore() : std::make_unique<meta::InMemoryTokenStore>();
    metaOptions.openUrl = m_options.openUrl;
    m_meta = std::make_unique<MetaServices>(*m_governor, *m_activity, *m_clock, std::move(metaOptions));
    connect(m_meta.get(), &MetaServices::spotifyStatusChanged, this, &Engine::spotifyStatusChanged);

    m_network = std::make_unique<NetworkService>(*m_governor, *m_activity, m_meta->tokens(), m_options.wallClock);
    connect(m_network.get(), &NetworkService::statusChanged, this, &Engine::networkStatusChanged);

    m_getTracks = std::make_unique<GetTracksService>(*m_db, m_options.wallClock);
    connect(m_getTracks.get(), &GetTracksService::linesReset, this, &Engine::getLinesReset);
    connect(m_getTracks.get(), &GetTracksService::lineChanged, this, &Engine::getLineChanged);

    m_inbox = std::make_unique<InboxService>(*m_db, m_options.library, m_options.trash);
    connect(m_inbox.get(), &InboxService::changed, this, &Engine::inboxChanged);
    connect(m_inbox.get(), &InboxService::finished, this, &Engine::inboxOperationFinished);

    pruneActivity();
    m_pruneTimer = new QTimer(this);
    m_pruneTimer->setInterval(kActivityPruneInterval);
    connect(m_pruneTimer, &QTimer::timeout, this, &Engine::pruneActivity);
    m_pruneTimer->start();
    m_budgetTimer = new QTimer(this);
    m_budgetTimer->setSingleShot(true);
    connect(m_budgetTimer, &QTimer::timeout, this, &Engine::reportSearchBudget);

    m_started = true;
    if (m_options.startPaused) {
        setPaused(true);
    }
    m_inbox->reconcile();
    m_meta->restore();
    m_network->start(m_options.network);
    Q_EMIT getLinesReset(m_getTracks->lines());
    qCInfo(lcEngine) << "engine started";
    Q_EMIT startFinished(true, QString());
    return true;
}

void Engine::stop()
{
    // Services go before the database they write to. Safe to call from the destructor at any stage of start().
    delete m_pruneTimer;
    m_pruneTimer = nullptr;
    delete m_budgetTimer;
    m_budgetTimer = nullptr;
    m_getTracks.reset();
    m_network.reset(); // before the token store it reads from (owned by m_meta)
    m_meta.reset();
    m_inbox.reset(); // waits for file work in flight and records it
    m_governorStore.reset();
    m_activity.reset();
    m_governor.reset();
    m_db.reset();
    if (m_started) {
        m_started = false;
        qCInfo(lcEngine) << "engine stopped";
    }
}

void Engine::addTracks(const QString &text)
{
    if (!m_started) {
        return;
    }
    const auto added = m_getTracks->add(text);
    const meta::spotify::PastedLinks links = meta::spotify::parsePastedLinks(text);
    QString note;
    if (!links.links.isEmpty()) {
        if (m_meta->spotify().isAvailable()) {
            note = links.links.size() == 1 ? tr("Importing the Spotify link…")
                                           : tr("Importing %1 Spotify links…").arg(links.links.size());
            m_meta->spotify().importLinks(links).then(this, [this](const meta::Result<meta::LinkImport> &result) {
                if (!m_started) {
                    return;
                }
                if (!result.ok()) {
                    Q_EMIT tracksAdded(0, 0, 0,
                                       tr("Spotify links couldn't be imported: %1").arg(result.error().message));
                    return;
                }
                const auto imported = m_getTracks->addReleases(result.value().tracks);
                QString message;
                if (!result.value().failed.isEmpty()) {
                    message = tr("%1 Spotify links couldn't be read.").arg(result.value().failed.size());
                }
                Q_EMIT tracksAdded(imported.added, imported.duplicates, 0, message);
            });
        } else {
            note = tr("Spotify links need Spotify set up in Settings; the other lines were added.");
        }
    }
    Q_EMIT tracksAdded(added.added, added.duplicates, added.unidentified, note);
}

void Engine::setPaused(bool paused)
{
    if (!m_governor || m_governor->isPaused() == paused) {
        return;
    }
    m_governor->setPaused(paused);
    m_activity->record(u"network"_s, QString(), paused ? u"paused"_s : u"resumed"_s,
                       paused ? tr("Network activity paused: no new searches or transfers.")
                              : tr("Network activity resumed."));
}

bool Engine::isPaused() const
{
    return m_governor && m_governor->isPaused();
}

void Engine::reportSearchBudget()
{
    if (!m_governor) {
        return;
    }
    const int available = m_governor->searchesAvailable();
    Q_EMIT searchBudgetChanged(available);
    if (available < governor::limits::kSearchBurst && m_budgetTimer) {
        // The next token's arrival, plus a millisecond so the bucket has really refilled when the timer fires.
        m_budgetTimer->start(m_governor->nextSearchIn() + 1ms);
    }
}

void Engine::pruneActivity()
{
    if (m_activity) {
        const int removed = m_activity->prune();
        if (removed > 0) {
            qCInfo(lcEngine) << "pruned" << removed << "activity lines";
        }
    }
}

} // namespace selecta::core
