// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Release.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

#include <chrono>
#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// A file on Soulseek chosen for a line: what to download, and why it was chosen.
struct FileChoice
{
    QString peer;
    QByteArray remotePath; // as the peer sent it
    qint64 size = 0;
    QString extension;          // lower case, "mp3", "flac"…
    std::optional<int> bitrate; // kbps
    std::optional<int> durationSec;
    std::optional<bool> vbr;
    std::optional<int> sampleRate;
    std::optional<int> bitDepth;
    bool freeSlot = false;
    int queueLength = 0;
    double fit = 0.0;         // 0…1, how well it matches the release
    bool fitsRelease = false; // same length within ±3 s and the version agrees (BATCH-5)

    bool operator==(const FileChoice &) const = default;
};

// One line of Get tracks (PRD BATCH-1…9): the track the user asked for, the release it was matched to, the file found
// for it on Soulseek, and how far it got.
struct GetLine
{
    enum class ReleaseState { Pending, Resolved, Ambiguous, None, Failed };
    enum class FindState { Idle, Queued, Searching, Ready, Check, NotFound, Owned, InInbox };
    enum class DownloadState { None, Queued, Transferring, Converting, Arrived, Failed };

    qint64 id = 0;
    int position = 0;
    QString raw; // as pasted
    QString dedupeKey;
    QString artist;
    QString title;
    std::optional<qint64> durationHintMs;
    QString isrc;
    QString labelHint;

    ReleaseState releaseState = ReleaseState::Pending;
    std::optional<meta::Release> release;
    QString releaseConfidence; // "exact", "confident"…
    bool resolving = false;    // a lookup is running now (not saved)

    FindState findState = FindState::Idle;
    std::optional<FileChoice> best;
    QList<FileChoice> alternatives; // other peers' files, best first (not saved: found again on the next search)
    bool ticked = false;
    DownloadState downloadState = DownloadState::None;
    QString message; // why a step failed, in words
    qint64 addedAt = 0;
    qint64 downloadId = 0;          // the download for the chosen file (not saved: restored from the downloads)
    double downloadProgress = -1.0; // 0…1 while transferring, -1 otherwise (not saved)
};

// Reads and writes Get tracks lines (table get_lines). Uses the Database on its owning thread; failures are reported
// through return values and the database's lastError().
class GetTracksStore
{
public:
    explicit GetTracksStore(storage::Database &db)
        : m_db(db)
    {}

    // Appends lines at the end of the list, skipping any whose dedupeKey is already there. Returns the new lines with
    // their ids and positions, in order.
    QList<GetLine> append(const QList<GetLine> &lines);
    [[nodiscard]] bool update(const GetLine &line);
    [[nodiscard]] bool remove(const QList<qint64> &ids);
    [[nodiscard]] bool clear();
    std::optional<GetLine> find(qint64 id);
    QList<GetLine> all(); // in list order
    int count();

private:
    storage::Database &m_db;
};

// FileChoice ↔ JSON text, for the database.
QByteArray fileChoiceToText(const FileChoice &choice);
std::optional<FileChoice> fileChoiceFromText(const QByteArray &text);

// About how long finding the rest of a Get tracks list takes (BATCH-4), at the governor's pace (D31): the searches
// still to start take what the budget holds beyond the reserve kept for searches typed by hand, then one per refill
// (the first when `nextIn` says), and the last one collects answers for a while. Zero when nothing is left. Pure.
std::chrono::milliseconds findingTimeLeft(int searchesToStart, int searchesCollecting, int searchesAvailable,
                                          std::chrono::milliseconds nextIn);

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::GetLine)
