// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Types.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

namespace selecta::share {

// The upload queue's policy (sharing note §5.2, §5.3, §9.2; ADR-012): which peer's file starts next, and which queue
// requests are refused. Pure bookkeeping: no I/O, no timers, no signals. Core checks the share index, starts and
// cancels the transfers, and tells the queue what happened.
//
// The order: privileged users' files come before everyone else's. Within each of the two groups, users take turns:
// the user whose last upload activity is oldest goes first (arriving in the queue, or an upload of theirs starting or
// ending, as Nicotine+ counts it), and each turn serves one file, the user's oldest. A user who just had a turn waits
// behind everyone who was waiting meanwhile, newcomers included. A file is queued until started(), then active until
// finished() or remove(). Limits count queued files only, as Nicotine+'s do.
//
// Threading: not thread-safe; one owner thread (core keeps it on the engine thread). Failure modes: none; refusals
// are answers (Decision), and calls about files the queue doesn't hold return false and change nothing. Memory:
// at most kMaxEntries files.
class UploadQueue
{
public:
    // Files the queue holds at most, queued and active, all users together.
    static constexpr int kMaxEntries = 10'000;

    struct Options
    {
        int slots = 3;             // uploads at once (Nicotine+'s default); 0 starts nothing
        int maxFilesPerUser = 100; // queued files per user, at most
        // Queued bytes per user: a request is refused once a user's queued files reach this (Nicotine+'s 10 000 MB),
        // so the last file accepted may take a queue past it.
        qint64 maxBytesPerUser = 10'000'000'000;

        friend bool operator==(const Options &, const Options &) = default;
    };

    struct Decision
    {
        bool accepted = false;
        bool repeat = false; // accepted because the user already has this file queued or active: nothing changed
        QString reason;      // refused: exactly "Too many files" or "Too many megabytes"

        friend bool operator==(const Decision &, const Decision &) = default;
    };

    struct Next
    {
        QString user;
        slsk::RemotePath path;
        quint64 size = 0;

        friend bool operator==(const Next &, const Next &) = default;
    };

    // Out-of-range options are clamped: slots and limits to 0 and up, maxFilesPerUser to kMaxEntries.
    UploadQueue();
    explicit UploadQueue(const Options &options);

    // New limits apply to later decisions; files already queued stay.
    void setOptions(const Options &options);
    const Options &options() const { return m_options; }

    // A peer asks for `path` (its raw bytes, the queue's key with `user`), `size` bytes. Accepted and appended to the
    // user's files, or refused with the exact standard reason: "Too many files" when the user already has
    // maxFilesPerUser files queued or the queue holds kMaxEntries, "Too many megabytes" when the user's queued files
    // reach maxBytesPerUser. A repeat of a file the user has queued or active is accepted without a second entry.
    // `privileged`: whether the user is privileged now (see setPrivileged()).
    Decision enqueue(const QString &user, const slsk::RemotePath &path, quint64 size, bool privileged);

    // The file to start now: nullopt when no slot is free (active() >= slots) or every user with queued files already
    // has an active upload (one per user: Soulseek NS can't take two from one source). Otherwise the first file in
    // the order (class comment) whose user has no active upload. Changes nothing: call started() once the upload is
    // started, or remove() when it can't be (the file isn't shared any more), or next() keeps returning it.
    std::optional<Next> next() const;

    // The upload of a queued file started: it becomes active and its user's turn is spent. False (nothing changes)
    // when the file isn't queued.
    bool started(const QString &user, const slsk::RemotePath &path);
    // The upload of an active file ended, whatever the outcome. False when the file isn't active.
    bool finished(const QString &user, const slsk::RemotePath &path);
    // Removes a file, queued or active (the peer withdrew it, core denied it). False when the queue doesn't hold it.
    bool remove(const QString &user, const slsk::RemotePath &path);
    // Removes all of a user's files (offline, banned). Returns the paths that were queued, oldest first, for core to
    // deny (active uploads are core's to cancel).
    QList<slsk::RemotePath> removeUser(const QString &user);

    // The users the server says are privileged (PrivilegedUsers). Replaces the previous set, and with it what
    // enqueue() said about every user: a user is privileged while in this set or since an enqueue() that said so.
    void setPrivileged(const QSet<QString> &users);

    // The 1-based place of a queued file in the order next() serves, as if uploads were started one at a time and
    // every active upload had just ended: what PlaceInQueueResponse reports. nullopt when the file isn't queued
    // (unknown, or active).
    std::optional<quint32> placeInQueue(const QString &user, const slsk::RemotePath &path) const;

    int queued() const { return m_queued; } // files waiting, all users (QueueLength in search answers)
    int active() const { return m_active; } // files uploading
    bool slotFree() const { return m_active < m_options.slots; }

private:
    struct Entry
    {
        slsk::RemotePath path;
        quint64 size = 0;
    };

    struct User
    {
        QString name;
        QList<Entry> queued; // oldest first
        QList<Entry> active;
        quint64 queuedBytes = 0;
        quint64 lastActivity = 0; // the clock when the user arrived or an upload of theirs started or ended
        bool privilegedHint = false;
    };

    bool isPrivileged(const User &user) const;
    // The order of turns: privileged first, then the oldest activity.
    bool goesBefore(const User &a, const User &b) const;
    // Forgets a user who holds no file any more.
    void dropIfIdle(QHash<QString, User>::iterator it);

    Options m_options;
    QHash<QString, User> m_users;
    QSet<QString> m_privileged;
    quint64 m_clock = 0; // ticks on every activity, so no two users share a stamp
    int m_queued = 0;
    int m_active = 0;
};

} // namespace selecta::share
