// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/Watcher.h"

#include "collection/detail/Walk.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>

#include <array>
#include <climits>
#include <cstdlib>
#include <span>

#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::collection {

namespace {

// A root as given (what changed() reports) and with its symlinks resolved (what the system reports).
struct Root
{
    QString given;
    QString real;
};

QString realPath(const QString &path)
{
    std::array<char, PATH_MAX> buffer{};
    if (::realpath(QFile::encodeName(path).constData(), buffer.data()) == nullptr) {
        return path;
    }
    return QFile::decodeName(buffer.data());
}

// An event path as a folder under its root as given; empty when it lies outside every root.
QString reportedFolder(const QString &path, const QList<Root> &roots)
{
    QString clean = QDir::cleanPath(path);
    for (const Root &root : roots) {
        if (detail::isUnder(clean, root.real)) {
            return root.given + clean.mid(root.real.size());
        }
        if (detail::isUnder(clean, root.given)) {
            return clean;
        }
    }
    return {};
}

QStringList givenRoots(const QList<Root> &roots)
{
    QStringList given;
    for (const Root &root : roots) {
        given.append(root.given);
    }
    return given;
}

} // namespace

// One platform mechanism. Created and destroyed on the watcher's thread; delivers through Watcher::deliver().
struct Watcher::Backend
{
    Backend(Watcher *owner, quint64 session)
        : m_owner(owner)
        , m_session(session)
    {}
    virtual ~Backend() = default;
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;
    Backend(Backend &&) = delete;
    Backend &operator=(Backend &&) = delete;

    // Starts watching; false with `error` set when nothing can be watched.
    virtual bool start(const QList<Root> &roots, QString &error) = 0;

protected:
    // On the watcher's thread.
    void deliver(const QStringList &folders) const { m_owner->deliver(m_session, folders); }
    static void deliverTo(Watcher *watcher, quint64 session, const QStringList &folders)
    {
        watcher->deliver(session, folders);
    }
    Watcher *owner() const { return m_owner; }
    quint64 session() const { return m_session; }

private:
    Watcher *m_owner = nullptr;
    quint64 m_session = 0;
};

namespace {

// QFileSystemWatcher on every folder under the roots (bounded), with a single-shot timer for the coalescing.
class PortableBackend final : public Watcher::Backend
{
public:
    PortableBackend(Watcher *owner, quint64 session)
        : Backend(owner, session)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(Watcher::kLatencyMs);
        QObject::connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_watcher,
                         [this](const QString &path) { changed(path); });
        QObject::connect(&m_timer, &QTimer::timeout, &m_watcher, [this] { flush(); });
    }

    bool start(const QList<Root> &roots, QString &error) override
    {
        m_roots = roots;
        for (const Root &root : roots) {
            addTree(root.given);
        }
        if (m_watcher.directories().isEmpty()) {
            error = u"the system refused to watch the folders"_s;
            return false;
        }
        return true;
    }

private:
    // The folder and its subfolders (hidden ones aside), up to kMaxPortableFolders in all.
    void addTree(const QString &folder)
    {
        QStringList folders{folder};
        QDirIterator it(folder, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (!path.contains(u"/."_s)) {
                folders.append(path);
            }
        }
        const QStringList watched = m_watcher.directories();
        QStringList fresh;
        for (const QString &path : std::as_const(folders)) {
            if (watched.size() + fresh.size() >= Watcher::kMaxPortableFolders) {
                break;
            }
            if (!watched.contains(path)) {
                fresh.append(path);
            }
        }
        if (!fresh.isEmpty()) {
            m_watcher.addPaths(fresh);
        }
    }

    void changed(const QString &path)
    {
        const QString folder = reportedFolder(path, m_roots);
        if (folder.isEmpty()) {
            return;
        }
        m_pending.insert(folder);
        if (QFileInfo(path).isDir()) {
            addTree(path); // folders created meanwhile
        }
        if (!m_timer.isActive()) {
            m_timer.start(); // from the first change of a burst
        }
    }

    void flush()
    {
        const QStringList folders(m_pending.cbegin(), m_pending.cend());
        m_pending.clear();
        deliver(folders);
    }

    QList<Root> m_roots;
    QFileSystemWatcher m_watcher;
    QTimer m_timer;
    QSet<QString> m_pending;
};

#ifdef Q_OS_MACOS

// FSEvents with directory-level events, the coalescing done by its latency, on a private serial dispatch queue.
class FsEventsBackend final : public Watcher::Backend
{
public:
    using Backend::Backend;
    ~FsEventsBackend() override { stop(); }
    FsEventsBackend(const FsEventsBackend &) = delete;
    FsEventsBackend &operator=(const FsEventsBackend &) = delete;
    FsEventsBackend(FsEventsBackend &&) = delete;
    FsEventsBackend &operator=(FsEventsBackend &&) = delete;

    bool start(const QList<Root> &roots, QString &error) override
    {
        m_roots = roots;
        CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, roots.size(), &kCFTypeArrayCallBacks);
        for (const Root &root : roots) {
            CFStringRef path = root.real.toCFString();
            CFArrayAppendValue(paths, path);
            CFRelease(path);
        }
        FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
        const CFTimeInterval latency = Watcher::kLatencyMs / 1000.0;
        m_stream =
            FSEventStreamCreate(nullptr, &FsEventsBackend::onEvents, &context, paths, kFSEventStreamEventIdSinceNow,
                                latency, kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagWatchRoot);
        CFRelease(paths);
        if (m_stream == nullptr) {
            error = u"FSEvents refused to watch the folders"_s;
            return false;
        }
        m_queue = dispatch_queue_create("org.selecta.collection.watcher", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(m_stream, m_queue);
        if (!FSEventStreamStart(m_stream)) {
            error = u"FSEvents couldn't start watching the folders"_s;
            stop();
            return false;
        }
        return true;
    }

private:
    // Stops the stream and waits until no callback runs: afterwards nothing touches this object or the watcher.
    void stop()
    {
        if (m_stream != nullptr) {
            FSEventStreamStop(m_stream);
            FSEventStreamInvalidate(m_stream);
            dispatch_sync_f(m_queue, nullptr, [](void * /*context*/) {}); // a barrier behind any running callback
            FSEventStreamRelease(m_stream);
            m_stream = nullptr;
        }
        if (m_queue != nullptr) {
            dispatch_release(m_queue);
            m_queue = nullptr;
        }
    }

    // On the dispatch queue: the roots are immutable while the stream runs; hand the folders to the watcher's thread.
    static bool hasFlag(FSEventStreamEventFlags flags, unsigned int flag) { return (flags & flag) != 0; }

    static void onEvents(ConstFSEventStreamRef /*stream*/, void *info, size_t count, void *eventPaths,
                         const FSEventStreamEventFlags *flags, const FSEventStreamEventId * /*ids*/)
    {
        const auto *self = static_cast<const FsEventsBackend *>(info);
        const auto *const paths = static_cast<CFArrayRef>(eventPaths);
        QStringList folders;
        bool everything = false;
        const std::span<const FSEventStreamEventFlags> eventFlags(flags, count);
        for (std::size_t i = 0; i < count; ++i) {
            const FSEventStreamEventFlags flag = eventFlags[i];
            if (hasFlag(flag, kFSEventStreamEventFlagUserDropped) ||
                hasFlag(flag, kFSEventStreamEventFlagKernelDropped)) {
                everything = true; // events were lost: everything may have changed
            }
            if (hasFlag(flag, kFSEventStreamEventFlagHistoryDone)) {
                continue;
            }
            const auto *const path = static_cast<CFStringRef>(CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
            const QString folder = reportedFolder(QString::fromCFString(path), self->m_roots);
            if (!folder.isEmpty()) {
                folders.append(folder);
            }
        }
        if (everything) {
            folders = givenRoots(self->m_roots);
        }
        if (folders.isEmpty()) {
            return;
        }
        Watcher *watcher = self->owner();
        const quint64 session = self->session();
        QMetaObject::invokeMethod(
            watcher, [watcher, session, folders] { Backend::deliverTo(watcher, session, folders); },
            Qt::QueuedConnection);
    }

    QList<Root> m_roots;
    FSEventStreamRef m_stream = nullptr;
    dispatch_queue_t m_queue = nullptr;
};

#endif

} // namespace

Watcher::Watcher(QObject *parent)
    : Watcher(Mechanism::Native, parent)
{}

Watcher::Watcher(Mechanism mechanism, QObject *parent)
    : QObject(parent)
    , m_mechanism(mechanism)
{}

Watcher::~Watcher()
{
    stop();
}

bool Watcher::watch(const QStringList &roots)
{
    stop();
    m_error.clear();
    QList<Root> existing;
    for (const QString &root : roots) {
        const QString given = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        if (QFileInfo(given).isDir()) {
            existing.append({given, QDir::cleanPath(realPath(given))});
        }
    }
    if (existing.isEmpty()) {
        m_error = u"none of the folders exists"_s;
        return false;
    }
    ++m_session;
#ifdef Q_OS_MACOS
    if (m_mechanism == Mechanism::Native) {
        m_backend = std::make_unique<FsEventsBackend>(this, m_session);
    }
#endif
    if (!m_backend) {
        m_backend = std::make_unique<PortableBackend>(this, m_session);
    }
    if (!m_backend->start(existing, m_error)) {
        m_backend.reset();
        return false;
    }
    m_roots = givenRoots(existing);
    return true;
}

void Watcher::stop()
{
    ++m_session;
    m_backend.reset(); // FSEvents: waits for a running callback; pending deliveries carry an old session
    m_roots.clear();
}

QStringList Watcher::roots() const
{
    return m_roots;
}

bool Watcher::isWatching() const
{
    return m_backend != nullptr;
}

QString Watcher::errorString() const
{
    return m_error;
}

void Watcher::deliver(quint64 session, const QStringList &folders)
{
    if (session != m_session || !m_backend) {
        return;
    }
    QStringList sorted = folders;
    sorted.sort();
    sorted.removeDuplicates();
    if (!sorted.isEmpty()) {
        Q_EMIT changed(sorted);
    }
}

} // namespace selecta::collection
