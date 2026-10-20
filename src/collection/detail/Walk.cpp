// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/detail/Walk.h"

#include "match/RemotePathParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::collection::detail {

namespace {

// A file system node: what "visited once" means.
struct NodeId
{
    dev_t device = 0;
    ino_t inode = 0;

    friend bool operator==(const NodeId &, const NodeId &) = default;
};

struct NodeIdHash
{
    std::size_t operator()(NodeId id) const noexcept
    {
        return std::hash<quint64>{}((static_cast<quint64>(id.device) << 40U) ^ static_cast<quint64>(id.inode));
    }
};

qint64 modifiedMs(const struct stat &status)
{
#ifdef Q_OS_DARWIN
    const struct timespec &time = status.st_mtimespec;
#else
    const struct timespec &time = status.st_mtim;
#endif
    return (static_cast<qint64>(time.tv_sec) * 1000) + (static_cast<qint64>(time.tv_nsec) / 1'000'000);
}

// The macOS "hidden" flag (chflags hidden): Finder doesn't show these.
bool hasHiddenFlag([[maybe_unused]] const struct stat &status)
{
#ifdef UF_HIDDEN
    return (status.st_flags & UF_HIDDEN) != 0;
#else
    return false;
#endif
}

// A file whose data lives only in the cloud (iCloud Drive, File Provider): reading it starts a download.
bool isDataless([[maybe_unused]] const struct stat &status)
{
#ifdef SF_DATALESS
    return (status.st_flags & SF_DATALESS) != 0;
#else
    return false;
#endif
}

QString errorText(int error)
{
    return qt_error_string(error); // strerror_r underneath: thread-safe
}

// The path with every symlink resolved, or an empty string when it can't be resolved (dangling, a loop).
QString resolved(const QByteArray &nativePath)
{
    std::array<char, PATH_MAX> buffer{};
    if (::realpath(nativePath.constData(), buffer.data()) == nullptr) {
        return {};
    }
    return QFile::decodeName(buffer.data());
}

bool insideAny(const QString &key, const QStringList &folders)
{
    return std::ranges::any_of(folders, [&key](const QString &folder) { return isUnder(key, folder); });
}

class Walker
{
public:
    Walker(const ScanOptions &options, const audio::CancellationFlag *cancel)
        : m_options(options)
        , m_cancel(cancel)
    {
        for (const QString &folder : options.excludedFolders) {
            if (!folder.isEmpty()) {
                m_excluded.append(comparisonKey(folder));
            }
        }
    }

    Walk run(const QStringList &roots)
    {
        for (const QString &root : roots) {
            m_walk.roots.append(QDir::cleanPath(QFileInfo(root).absoluteFilePath()));
        }
        // Symlink targets must resolve inside one of these (the roots with their own symlinks resolved).
        for (const QString &root : std::as_const(m_walk.roots)) {
            const QString real = resolved(QFile::encodeName(root));
            m_realRoots.append(comparisonKey(real.isEmpty() ? root : real));
        }
        for (qsizetype r = 0; r < m_walk.roots.size() && !stopped(); ++r) {
            walkRoot(r);
        }
        return std::move(m_walk);
    }

private:
    struct Pending
    {
        QString path;
        qsizetype root = 0;
        int depth = 0;
    };

    bool stopped()
    {
        if (m_walk.cancelled || !m_walk.complete) {
            return true;
        }
        if (m_cancel != nullptr && m_cancel->isCancelled()) {
            m_walk.cancelled = true;
            m_walk.complete = false;
            return true;
        }
        return false;
    }

    void blind(const QString &path, const QString &reason)
    {
        m_walk.unreadable.append({path, reason});
        m_walk.blindFolders.append(comparisonKey(path));
    }

    void walkRoot(qsizetype root)
    {
        const QString &path = m_walk.roots.at(root);
        struct stat status{};
        if (::stat(QFile::encodeName(path).constData(), &status) != 0) {
            blind(path, u"the folder isn't available: %1"_s.arg(errorText(errno)));
            return;
        }
        if (!S_ISDIR(status.st_mode)) {
            blind(path, u"not a folder"_s);
            return;
        }
        if (!m_visitedFolders.insert({status.st_dev, status.st_ino}).second) {
            return; // the same folder as an earlier root, or inside one: already walked
        }
        std::vector<Pending> stack{{path, root, 0}};
        while (!stack.empty() && !stopped()) {
            const Pending folder = std::move(stack.back());
            stack.pop_back();
            std::vector<Pending> children = listFolder(folder);
            // Depth first, names in order: push the children in reverse so the first name is walked first.
            for (Pending &child : std::views::reverse(children)) {
                stack.push_back(std::move(child));
            }
        }
    }

    std::vector<Pending> listFolder(const Pending &folder)
    {
        std::vector<Pending> children;
        const QByteArray nativePath = QFile::encodeName(folder.path);
        DIR *directory = ::opendir(nativePath.constData());
        if (directory == nullptr) {
            blind(folder.path, u"can't list the folder: %1"_s.arg(errorText(errno)));
            return children;
        }
        std::vector<std::string> names;
        // One DIR stream per call, used by this thread only: readdir is safe across streams (macOS, glibc).
        while (const dirent *entry = ::readdir(directory)) { // NOLINT(concurrency-mt-unsafe)
            const char *name = entry->d_name;
            if (name[0] == '.') {
                continue; // ".", "..", dot files and folders, AppleDouble "._x"
            }
            names.emplace_back(name);
        }
        std::ranges::sort(names);
        const auto index = static_cast<qsizetype>(m_walk.folders.size());
        m_walk.folders.push_back({folder.path, folder.root, {}});
        const int descriptor = ::dirfd(directory);
        for (const std::string &name : names) {
            if (stopped()) {
                break;
            }
            visitEntry(descriptor, folder, index, name, children);
        }
        ::closedir(directory);
        return children;
    }

    void visitEntry(int descriptor, const Pending &folder, qsizetype index, const std::string &name,
                    std::vector<Pending> &children)
    {
        const QString childName = QFile::decodeName(name.c_str());
        const QString childPath = folder.path + u'/' + childName;
        struct stat status{};
        if (::fstatat(descriptor, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) { // gone since the listing: nothing to report
                m_walk.unreadable.append({childPath, u"can't read its attributes: %1"_s.arg(errorText(errno))});
                m_walk.blindFolders.append(comparisonKey(childPath));
            }
            return;
        }
        if (S_ISLNK(status.st_mode)) {
            // Followed only into the roots; the (device, inode) sets below keep loops and second paths out.
            const QByteArray nativeChild = QFile::encodeName(childPath);
            const QString target = resolved(nativeChild);
            if (target.isEmpty() || !insideAny(comparisonKey(target), m_realRoots) ||
                ::stat(nativeChild.constData(), &status) != 0) {
                return;
            }
        }
        if (S_ISDIR(status.st_mode)) {
            visitFolder(folder, childPath, status, children);
        } else if (S_ISREG(status.st_mode)) {
            visitFile(index, childName, childPath, status);
        }
        // Anything else (FIFOs, sockets, devices) is never a track.
    }

    void visitFolder(const Pending &folder, const QString &path, const struct stat &status,
                     std::vector<Pending> &children)
    {
        if (hasHiddenFlag(status) || insideAny(comparisonKey(path), m_excluded)) {
            return;
        }
        if (!m_visitedFolders.insert({status.st_dev, status.st_ino}).second) {
            return; // a loop, or a second path to a folder already walked
        }
        if (folder.depth + 1 > m_options.maxDepth) {
            blind(path, u"nested more than %1 folders deep"_s.arg(m_options.maxDepth));
            return;
        }
        children.push_back({path, folder.root, folder.depth + 1});
    }

    void visitFile(qsizetype index, const QString &name, const QString &path, const struct stat &status)
    {
        if (!isAudioName(name) || isTemporaryName(name) || hasHiddenFlag(status)) {
            return;
        }
        if (!m_visitedFiles.insert({status.st_dev, status.st_ino}).second) {
            return; // a second path to a file already found (a symlink inside the roots)
        }
        const QString key = comparisonKey(path);
        if (isDataless(status)) {
            m_walk.unreadable.append({path, u"an iCloud placeholder: its audio isn't on this Mac"_s});
            m_walk.presentUnread.insert(key);
            return;
        }
        if (std::cmp_greater_equal(m_walk.files.size(), m_options.maxFiles)) {
            m_walk.unreadable.append({m_walk.roots.at(m_walk.folders[static_cast<std::size_t>(index)].root),
                                      u"more than %1 audio files: the rest weren't scanned"_s.arg(m_options.maxFiles)});
            m_walk.complete = false;
            return;
        }
        m_walk.files.push_back({{path, static_cast<qint64>(status.st_size), modifiedMs(status)}, key, index});
        m_walk.folders[static_cast<std::size_t>(index)].audioNames.append(name);
    }

    const ScanOptions &m_options;
    const audio::CancellationFlag *m_cancel = nullptr;
    QStringList m_excluded;
    QStringList m_realRoots;
    std::unordered_set<NodeId, NodeIdHash> m_visitedFolders;
    std::unordered_set<NodeId, NodeIdHash> m_visitedFiles;
    Walk m_walk;
};

} // namespace

bool isHiddenName(QStringView name)
{
    return name.startsWith(u'.');
}

bool isTemporaryName(QStringView name)
{
    static constexpr std::array<QStringView, 8> kSuffixes{u".selecta-part", u".part",       u".partial",  u".tmp",
                                                          u".temp",         u".crdownload", u".download", u".!qB"};
    if (name.startsWith(u"~$") || name.endsWith(u'~')) {
        return true;
    }
    return std::ranges::any_of(kSuffixes,
                               [name](QStringView suffix) { return name.endsWith(suffix, Qt::CaseInsensitive); });
}

bool isAudioName(QStringView name)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot <= 0 || dot + 1 >= name.size()) {
        return false;
    }
    return match::RemotePathParser::fileKind(name.sliced(dot + 1)) == match::FileKind::Audio;
}

bool isUnder(QStringView key, QStringView folder)
{
    if (folder.isEmpty()) {
        return false;
    }
    if (folder.endsWith(u'/')) { // the file system root
        return key.startsWith(folder);
    }
    return key == folder || (key.size() > folder.size() && key.startsWith(folder) && key[folder.size()] == u'/');
}

Walk walk(const QStringList &roots, const ScanOptions &options, const audio::CancellationFlag *cancel)
{
    return Walker(options, cancel).run(roots);
}

} // namespace selecta::collection::detail
