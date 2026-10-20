// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace selecta::collection {

// Watches the collection roots and says which folders changed (PRD LIB-2): FSEvents on macOS, QFileSystemWatcher
// elsewhere. Events are hints, never truth (note §12): core rescans the folders it is given.
//
// Changes are coalesced: changed() comes about kLatencyMs after the first change of a burst, with every folder that
// changed meanwhile. Folders are reported under the roots as they were given (a root behind a symlink, such as macOS'
// /var → /private/var, is reported as given). When events were dropped, or a root itself was moved, deleted or
// created, the roots concerned are reported: rescan everything under them.
//
// Threading: lives on the thread that created it (or was moved to), which must run an event loop; changed() is emitted
// there. Ownership: a plain QObject. Failure modes: watch() returns false when no root can be watched (missing roots,
// the system refused); errorString() says why.
class Watcher : public QObject
{
    Q_OBJECT

public:
    static constexpr int kLatencyMs = 2000;
    static constexpr int kMaxPortableFolders = 8192; // Portable watches at most this many folders (then the roots only)

    enum class Mechanism {
        Native,   // FSEvents on macOS; QFileSystemWatcher elsewhere
        Portable, // QFileSystemWatcher on every folder under the roots, on every platform (tests, odd volumes)
    };

    explicit Watcher(QObject *parent = nullptr);
    explicit Watcher(Mechanism mechanism, QObject *parent = nullptr);
    ~Watcher() override;
    Watcher(const Watcher &) = delete;
    Watcher &operator=(const Watcher &) = delete;
    Watcher(Watcher &&) = delete;
    Watcher &operator=(Watcher &&) = delete;

    // Starts watching `roots` (absolute folders), replacing what was watched before. Roots that don't exist are left
    // out; returns false (and watches nothing) when none remains or the system refused.
    bool watch(const QStringList &roots);
    // Stops watching; no changed() is emitted afterwards.
    void stop();

    QStringList roots() const;
    bool isWatching() const;
    QString errorString() const;
    Mechanism mechanism() const { return m_mechanism; }

    struct Backend; // one per platform mechanism

Q_SIGNALS:
    // Folders under the roots where something changed, sorted, each once. Rescan each one recursively.
    void changed(const QStringList &folders);

private:
    friend struct Backend;
    void deliver(quint64 session, const QStringList &folders);

    Mechanism m_mechanism = Mechanism::Native;
    std::unique_ptr<Backend> m_backend;
    QStringList m_roots;
    QString m_error;
    quint64 m_session = 0; // bumped by watch() and stop(): late events of an earlier session are dropped
};

} // namespace selecta::collection
