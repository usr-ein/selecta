// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ActivityLog.h"
#include "core/Engine.h"

#include <QObject>
#include <QPointer>
#include <QThread>

namespace selecta::core {

// Runs the Engine on its own thread and is the UI's only way to reach it. Lives on the UI thread.
//
// Calls are posted to the engine thread and return at once; results come back as this object's signals, delivered on
// the UI thread. State the UI shows (paused, search budget) is cached here from those signals, so reading it never
// blocks. stop() and the destructor wait for the engine thread to finish.
class EngineHost : public QObject
{
    Q_OBJECT

public:
    explicit EngineHost(EngineOptions options, QObject *parent = nullptr);
    ~EngineHost() override;
    EngineHost(const EngineHost &) = delete;
    EngineHost &operator=(const EngineHost &) = delete;
    EngineHost(EngineHost &&) = delete;
    EngineHost &operator=(EngineHost &&) = delete;

    // Starts the engine thread; started() reports whether the engine came up. Does nothing if already running.
    void start();
    // Stops the engine and joins its thread. Blocks until both are done; safe to call more than once.
    void stop();
    bool isRunning() const { return m_thread.isRunning(); }

    bool isPaused() const { return m_paused; }
    int searchesAvailable() const { return m_searchesAvailable; }

    void setPaused(bool paused);

    // --- the Inbox ---
    void setLibraryConfig(const LibraryConfig &config);
    void refreshInbox();
    void fileInboxItems(const QList<qint64> &ids);
    void trashInboxItems(const QList<qint64> &ids);
    QList<InboxEntry> inboxEntries() const { return m_inboxEntries; }

    // --- Spotify (Settings → Spotify) ---
    void setSpotifyCredentials(const QString &clientId, const QByteArray &secret, bool keepSavedSecret);
    void signInToSpotify();
    void signOutOfSpotify();
    SpotifyStatus spotifyStatus() const { return m_spotifyStatus; }

    // --- Get tracks ---
    void addTracks(const QString &text);
    void removeTracks(const QList<qint64> &ids);
    void clearTracks();
    void tickTracks(const QList<qint64> &ids, bool ticked);
    void retryTracks(const QList<qint64> &ids);
    QList<GetLine> getLines() const { return m_getLines; }

    // Runs `work(engine)` on the engine thread, if the engine is running. For services that have no dedicated
    // method here yet; `work` must not capture anything owned by the UI thread by reference.
    template <typename Work>
    void post(Work work)
    {
        if (m_engine) {
            QMetaObject::invokeMethod(
                m_engine.data(), [engine = m_engine.data(), work = std::move(work)] { work(*engine); },
                Qt::QueuedConnection);
        }
    }

Q_SIGNALS:
    void started(bool ok, const QString &error);
    void pausedChanged(bool paused);
    void searchBudgetChanged(int available);
    void activityRecorded(const selecta::core::ActivityEntry &entry);
    void inboxChanged(const QList<selecta::core::InboxEntry> &entries);
    void inboxOperationFinished(qint64 id, bool ok, const QString &message);
    void spotifyStatusChanged(const selecta::core::SpotifyStatus &status);
    void getLinesReset(const QList<selecta::core::GetLine> &lines);
    void getLineChanged(const selecta::core::GetLine &line);
    // What adding did, for a short message: lines added, already there, unidentified, and a note.
    void tracksAdded(int added, int duplicates, int unidentified, const QString &note);

private:
    EngineOptions m_options;
    QThread m_thread;
    QPointer<Engine> m_engine;
    bool m_paused = false;
    int m_searchesAvailable = 0;
    QList<InboxEntry> m_inboxEntries;
    SpotifyStatus m_spotifyStatus;
    QList<GetLine> m_getLines;
};

} // namespace selecta::core
