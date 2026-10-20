// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"
#include "core/Downloads.h"
#include "library/FileOps.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

class ActivityLog;
class InboxService;
class MetaServices;

// How an arrival ended.
struct ArrivalResult
{
    bool ok = false;
    qint64 inboxItem = 0;
    QString message; // where the file went ("Singles/Okuma_Drift_-_Neon_Rain.mp3"), or why it didn't
    QString warning; // e.g. kept below the Inbox format
};

// The tags a release gives a file (META-4, D30): title, artists, album, numbers, date, ISRC, label, genres and the
// source link, all marked Provenance::Release. Pure.
convert::MetadataModel tagsFromRelease(const meta::Release &release);

// The arrival pipeline (PRD DL-3, D29, D30) on the engine thread: a completed download becomes an Inbox item. The
// release's tags are completed first (label and genre from MusicBrainz by ISRC, the cover), then a worker converts the
// file to the Inbox format (MP3 320 CBR, ID3v2.3, cover embedded), verified, or keeps lossy audio that can't be
// improved as it is with fresh tags; publishes it into Inbox/Singles under the naming template; and disposes of the
// original: a converted lossless original goes to the Trash, a copied one is deleted. The Inbox item and the history
// entry are recorded back on the engine thread.
//
// Threading: engine thread; the file work runs on a pool of its own, one conversion per worker. Results come back
// through a mutex-guarded mailbox. Ownership: the database, Inbox service, activity log and metadata services outlive
// it; the destructor cancels work in flight and waits for it.
class ArrivalService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;

    struct Options
    {
        QString stagingDir;         // converted files wait here until they are published
        QString encodedBy;          // ENCODEDBY, e.g. "Selecta 0.1.0"
        std::optional<int> session; // recorded with Inbox items and history entries
        int maxConcurrent = 0;      // 0: half the cores, at most 4
        int metadataTimeoutMs = 20'000;
    };

    ArrivalService(storage::Database &db, InboxService &inbox, ActivityLog &activity,
                   library::FileOps::TrashFunction trash, Options options, WallClock now = {},
                   QObject *parent = nullptr);
    ~ArrivalService() override;
    ArrivalService(const ArrivalService &) = delete;
    ArrivalService &operator=(const ArrivalService &) = delete;
    ArrivalService(ArrivalService &&) = delete;
    ArrivalService &operator=(ArrivalService &&) = delete;

    // Where label, genre and covers come from. Without it, files are tagged from the release as it is.
    void setMeta(MetaServices *meta) { m_meta = meta; }

    // Takes a completed download (download.localPath). finished() follows, once.
    void start(const Download &download);
    // Stops an arrival before its file is published; finished() then reports it cancelled. Unknown ids are ignored.
    void cancel(qint64 downloadId);
    bool isBusy() const { return !m_jobs.isEmpty(); }
    // Blocks until the workers are done and their results recorded. For shutdown and tests.
    void waitForIdle();

Q_SIGNALS:
    void finished(qint64 downloadId, const selecta::core::ArrivalResult &result);

private Q_SLOTS:
    void drain();

private:
    struct Job;
    struct Outcome;

    void lookUpTags(const std::shared_ptr<Job> &job);
    void fetchCover(const std::shared_ptr<Job> &job);
    void runWorker(const std::shared_ptr<Job> &job);
    static Outcome work(const Job &job);
    void record(const Outcome &outcome);

    storage::Database &m_db;
    InboxService &m_inbox;
    ActivityLog &m_activity;
    library::FileOps::TrashFunction m_trash;
    Options m_options;
    WallClock m_now;
    MetaServices *m_meta = nullptr;
    QHash<qint64, std::shared_ptr<Job>> m_jobs; // by download id, until finished() is emitted
    QThreadPool m_pool;
    std::mutex m_mutex;
    QList<Outcome> m_outcomes; // guarded by m_mutex: filled by workers, drained on the engine thread
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ArrivalResult)
