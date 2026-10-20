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

    explicit Watcher(QObject *parent = nullptr);
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

    struct Backend; // the platform's watcher (FSEvents, QFileSystemWatcher)

Q_SIGNALS:
    // Folders under the roots where something changed, sorted, each once. Rescan each one recursively.
    void changed(const QStringList &folders);

private:
    std::unique_ptr<Backend> m_backend;
    QStringList m_roots;
    QString m_error;
};

} // namespace selecta::collection
