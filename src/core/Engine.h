// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ActivityLog.h"
#include "core/InboxService.h"
#include "core/MetaServices.h"
#include "governor/Governor.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QTimer;

namespace selecta::base {
class Clock;
}
namespace selecta::storage {
class Database;
}

namespace selecta::core {

class GovernorStore;

struct EngineOptions
{
    QString databasePath;               // the engine opens its own connection; ":memory:" in tests
    bool startPaused = false;           // the user's "Pause network activity" from the last run (SAFE-5)
    const base::Clock *clock = nullptr; // default: a steady clock owned by the engine
    std::function<qint64()> wallClock;  // Unix time, ms; default: the system clock
    governor::Governor::Options governor;
    LibraryConfig library;
    library::FileOps::TrashFunction trash; // default: the macOS Trash; tests inject their own
    // Where secrets live: the app passes the Keychain; default: an in-memory store (tests).
    std::function<std::unique_ptr<meta::TokenStore>()> tokenStore;
    // Opens a URL in the browser, for the Spotify sign-in; called on the engine thread.
    std::function<bool(const QUrl &)> openUrl;
};

// The services on the engine thread (PLAN §2.3, the network thread): the governor and its saved state and the
// activity log, then the Soulseek client, metadata services, searches, transfers and the arrival pipeline as they land.
//
// Threading: created on the UI thread and moved to the engine thread by EngineHost before start(); from then on every
// method runs on the engine thread. Other threads talk to it only through EngineHost.
// Ownership: owns every service; services are created in start() so they live on the engine thread.
class Engine : public QObject
{
    Q_OBJECT

public:
    explicit Engine(EngineOptions options, QObject *parent = nullptr);
    ~Engine() override;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&) = delete;
    Engine &operator=(Engine &&) = delete;

    // Opens the database connection, brings the schema up to date and restores saved state, then emits
    // startFinished(). Returns false, with error() set, if the engine can't run.
    bool start();
    // Stops every service and closes the database. Idempotent; the destructor calls it.
    void stop();
    bool isStarted() const { return m_started; }
    QString error() const { return m_error; }

    // "Pause network activity" (SAFE-5): no new Soulseek operations until resumed.
    void setPaused(bool paused);
    bool isPaused() const;

    // For services and tests on the engine thread. Valid between start() and stop().
    governor::Governor &governor() { return *m_governor; }
    ActivityLog &activity() { return *m_activity; }
    InboxService &inbox() { return *m_inbox; }
    MetaServices &meta() { return *m_meta; }
    storage::Database &database() { return *m_db; }

Q_SIGNALS:
    void startFinished(bool ok, const QString &error);
    void pausedChanged(bool paused);
    void searchBudgetChanged(int available);
    void activityRecorded(const selecta::core::ActivityEntry &entry);
    void inboxChanged(const QList<selecta::core::InboxEntry> &entries);
    void inboxOperationFinished(qint64 id, bool ok, const QString &message);
    void spotifyStatusChanged(const selecta::core::SpotifyStatus &status);

private:
    bool fail(const QString &error);
    void pruneActivity();
    // Reports the search budget now and again whenever it refills, until it's full.
    void reportSearchBudget();

    EngineOptions m_options;
    std::unique_ptr<base::Clock> m_ownClock;
    const base::Clock *m_clock = nullptr;
    std::unique_ptr<storage::Database> m_db;
    std::unique_ptr<governor::Governor> m_governor;
    std::unique_ptr<GovernorStore> m_governorStore;
    std::unique_ptr<ActivityLog> m_activity;
    std::unique_ptr<InboxService> m_inbox;
    std::unique_ptr<MetaServices> m_meta;
    QTimer *m_pruneTimer = nullptr;
    QTimer *m_budgetTimer = nullptr;
    bool m_started = false;
    QString m_error;
};

} // namespace selecta::core
