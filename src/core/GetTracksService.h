// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"
#include "meta/ReleaseResolver.h"

#include <QFuture>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// Get tracks (PRD D27, BATCH-1…9) on the engine thread: the list the user pasted, each line matched to a release
// before any Soulseek search (BATCH-5), then found, previewed and downloaded. The lines live in the database, so the
// list survives a restart; a line resolved once stays resolved.
//
// Threading: engine thread. Resolutions run asynchronously, at most kMaxResolving at a time; their results come back
// on this thread. Changes are published as lineChanged() for one line and linesReset() when lines come or go.
class GetTracksService : public QObject
{
    Q_OBJECT

public:
    // How a line becomes release candidates: meta's ReleaseResolver in the app, a fake in tests.
    using ResolveFunction = std::function<QFuture<meta::Result<meta::Resolution>>(const meta::ResolveQuery &)>;

    static constexpr int kMaxResolving = 3;

    struct AddResult
    {
        int added = 0;
        int duplicates = 0;   // already in the list, or twice in what was pasted
        int unidentified = 0; // "ID - ID" and the like: never searched (note §9.1)
    };

    explicit GetTracksService(storage::Database &db, std::function<qint64()> wallClock = {}, QObject *parent = nullptr);
    ~GetTracksService() override;
    GetTracksService(const GetTracksService &) = delete;
    GetTracksService &operator=(const GetTracksService &) = delete;
    GetTracksService(GetTracksService &&) = delete;
    GetTracksService &operator=(GetTracksService &&) = delete;

    // Sets how releases are found and starts resolving pending lines; an empty function pauses resolution.
    void setResolver(ResolveFunction resolve);

    // Parses pasted text (one track per line) and appends the new lines (BATCH-1).
    AddResult add(const QString &text);
    // Appends tracks that already carry their release (a Spotify playlist or album, META-3).
    AddResult addReleases(const QList<meta::Release> &tracks);
    void remove(const QList<qint64> &ids);
    void clear();
    void setTicked(const QList<qint64> &ids, bool ticked);
    // Resolves these lines again (after a failure, or to look for a better release).
    void retry(const QList<qint64> &ids);
    // The user picked a release among the candidates.
    void chooseRelease(qint64 id, const meta::Release &release);

    QList<GetLine> lines() const { return m_lines; }
    int resolvingCount() const { return static_cast<int>(m_resolving.size()); }

Q_SIGNALS:
    void linesReset(const QList<selecta::core::GetLine> &lines);
    void lineChanged(const selecta::core::GetLine &line);

private:
    void load();
    void resolveNext();
    void resolved(qint64 id, const meta::Result<meta::Resolution> &result);
    void store(const GetLine &line);
    GetLine *lineWithId(qint64 id);

    storage::Database &m_db;
    std::function<qint64()> m_now;
    GetTracksStore m_store;
    ResolveFunction m_resolve;
    QList<GetLine> m_lines;                                             // in list order, mirrors the database
    QHash<qint64, QFuture<meta::Result<meta::Resolution>>> m_resolving; // line id → resolution; cancelled if it goes
};

} // namespace selecta::core
