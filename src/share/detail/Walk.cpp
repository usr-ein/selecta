// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/Walk.h"

#include "collection/Scanner.h"
#include "share/ShareIndex.h"
#include "share/detail/Text.h"

#include <QFile>
#include <QSet>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::share::detail {

namespace {

// A file system node: what "visited once" means.
struct NodeId
{
    quint64 device = 0;
    quint64 inode = 0;

    friend bool operator==(const NodeId &, const NodeId &) = default;
};

struct NodeIdHash
{
    std::size_t operator()(NodeId id) const noexcept { return std::hash<quint64>{}((id.device << 40U) ^ id.inode); }
};

NodeId nodeOf(const struct stat &status)
{
    return {static_cast<quint64>(status.st_dev), static_cast<quint64>(status.st_ino)};
}

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

// For messages: the local path as the user knows it.
QString displayPath(const QByteArray &nativePath)
{
    return QFile::decodeName(nativePath);
}

// The comparison key of the path with every symlink resolved, or of the path itself when it can't be resolved.
QString resolvedKey(const QString &path)
{
    std::array<char, PATH_MAX> buffer{};
    if (::realpath(QFile::encodeName(path).constData(), buffer.data()) == nullptr) {
        return collection::comparisonKey(path);
    }
    return collection::comparisonKey(QFile::decodeName(buffer.data()));
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

class Walker
{
public:
    Walker(const QStringList &excludedFolders, AttributeSource &attributes, const audio::CancellationFlag *cancel)
        : m_attributes(attributes)
        , m_cancel(cancel)
    {
        for (const QString &folder : excludedFolders) {
            if (folder.isEmpty()) {
                continue;
            }
            struct stat status{};
            if (::stat(QFile::encodeName(folder).constData(), &status) == 0 && S_ISDIR(status.st_mode)) {
                m_excludedNodes.insert(nodeOf(status));
            }
            m_excludedKeys.append(resolvedKey(folder));
        }
    }

    Walk run(const QList<WalkRoot> &roots)
    {
        for (const WalkRoot &root : roots) {
            if (stopped()) {
                break;
            }
            walkRoot(root);
        }
        return std::move(m_walk);
    }

private:
    struct Pending
    {
        QByteArray nativePath;
        QString keyPath; // the path as collection::scan() spells it
        QString virtualPath;
        qsizetype virtualBytes = 0; // UTF-8 bytes of virtualPath
        QString segment;
        qsizetype parent = -1;
        int depth = 0;
    };

    // What one folder listing collects.
    struct Listing
    {
        QSet<QString> folderNames; // virtual names taken by subfolders
        QSet<QByteArray> fileNames; // virtual names taken by files
        std::vector<WalkedFile> files;
        std::vector<Pending> children;
    };

    bool stopped()
    {
        if (m_walk.cancelled || m_full) {
            return true;
        }
        if (m_cancel != nullptr && m_cancel->isCancelled()) {
            m_walk.cancelled = true;
            return true;
        }
        return false;
    }

    void report(const QString &path, const QString &reason) { m_walk.unreadable.append({path, reason}); }

    void markFull(const QString &reason)
    {
        if (!m_full) {
            m_full = true;
            report(m_root, reason);
        }
    }

    void walkRoot(const WalkRoot &root)
    {
        const QByteArray native = QFile::encodeName(root.path);
        struct stat status{};
        if (::stat(native.constData(), &status) != 0) {
            report(root.path, u"the folder isn't available: %1"_s.arg(errorText(errno)));
            return;
        }
        if (!S_ISDIR(status.st_mode)) {
            report(root.path, u"not a folder"_s);
            return;
        }
        const QString key = resolvedKey(root.path);
        if (m_excludedNodes.contains(nodeOf(status)) ||
            std::ranges::any_of(m_excludedKeys, [&key](const QString &excluded) { return isUnder(key, excluded); })) {
            return;
        }
        if (!m_visitedFolders.insert(nodeOf(status)).second) {
            return; // the same folder as an earlier root, or inside one: its files are shared there
        }
        m_root = root.path;
        const QByteArray aliasBytes = root.alias.toUtf8();
        std::vector<Pending> stack{{native, root.path, root.alias, aliasBytes.size(), root.alias, -1, 0}};
        while (!stack.empty() && !stopped()) {
            Pending folder = std::move(stack.back());
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
        const auto index = static_cast<qsizetype>(m_walk.folders.size());
        m_walk.folders.push_back({folder.nativePath, folder.virtualPath, folder.segment, folder.parent, {}});
        m_poolBytes += folder.virtualBytes + folder.nativePath.size();
        Listing listing;
        DIR *directory = ::opendir(folder.nativePath.constData());
        if (directory == nullptr) {
            report(displayPath(folder.nativePath), u"can't list the folder: %1"_s.arg(errorText(errno)));
            return {};
        }
        std::vector<std::string> names;
        // One DIR stream per call, used by this thread only: readdir is safe across streams (macOS, glibc).
        errno = 0;
        while (const dirent *entry = ::readdir(directory)) { // NOLINT(concurrency-mt-unsafe)
            if (entry->d_name[0] != '.') { // ".", "..", dot files and folders, AppleDouble "._x"
                names.emplace_back(entry->d_name);
            }
            errno = 0;
        }
        if (errno != 0) {
            report(displayPath(folder.nativePath), u"the folder couldn't be listed whole: %1"_s.arg(errorText(errno)));
        }
        std::ranges::sort(names);
        const int descriptor = ::dirfd(directory);
        for (const std::string &name : names) {
            if (stopped()) {
                break;
            }
            visitEntry(descriptor, folder, index, QByteArray(name.data(), static_cast<qsizetype>(name.size())),
                       listing);
        }
        ::closedir(directory);
        std::ranges::sort(listing.files, {}, &WalkedFile::virtualName);
        m_walk.folders[static_cast<std::size_t>(index)].files = std::move(listing.files);
        return std::move(listing.children);
    }

    void visitEntry(int descriptor, const Pending &folder, qsizetype index, const QByteArray &name, Listing &listing)
    {
        struct stat status{};
        if (::fstatat(descriptor, name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) { // gone since the listing: nothing to report
                report(displayPath(folder.nativePath + '/' + name),
                       u"can't read its attributes: %1"_s.arg(errorText(errno)));
            }
            return;
        }
        if (S_ISDIR(status.st_mode)) {
            visitFolder(folder, index, name, status, listing);
        } else if (S_ISREG(status.st_mode)) {
            visitFile(folder, name, status, listing);
        }
        // Symlinks are never followed; FIFOs, sockets and devices are never shared.
    }

    void visitFolder(const Pending &folder, qsizetype index, const QByteArray &name, const struct stat &status,
                     Listing &listing)
    {
        if (hasHiddenFlag(status) || m_excludedNodes.contains(nodeOf(status))) {
            return;
        }
        const QByteArray nativePath = folder.nativePath + '/' + name;
        const LocalName local = localName(name);
        if (!local.problem.isEmpty()) {
            report(displayPath(nativePath), local.problem);
            return;
        }
        if (isJunkFolderName(local.virtualName)) {
            return;
        }
        if (folder.depth + 1 > kMaxDepth) {
            report(displayPath(nativePath), u"nested more than %1 folders deep"_s.arg(kMaxDepth));
            return;
        }
        const qsizetype virtualBytes = folder.virtualBytes + 1 + local.virtualName.toUtf8().size();
        if (virtualBytes > kMaxVirtualPathBytes) {
            report(displayPath(nativePath), u"its path is too long for peers"_s);
            return;
        }
        if (listing.folderNames.contains(local.virtualName)) {
            report(displayPath(nativePath), u"another folder beside it has the same name for peers"_s);
            return;
        }
        if (!m_visitedFolders.insert(nodeOf(status)).second) {
            return; // a second path to a folder already walked (a hard-linked folder)
        }
        listing.folderNames.insert(local.virtualName);
        listing.children.push_back({nativePath, folder.keyPath + u'/' + local.key,
                                    folder.virtualPath + u'\\' + local.virtualName, virtualBytes, local.virtualName,
                                    index, folder.depth + 1});
    }

    void visitFile(const Pending &folder, const QByteArray &name, const struct stat &status, Listing &listing)
    {
        if (hasHiddenFlag(status) || status.st_size <= 0) {
            return;
        }
        // The kind first, from a lenient decoding: a file that wouldn't be shared anyway is never reported.
        const QString lenient = QString::fromUtf8(name);
        const FileKind kind = fileKind(lenient);
        if (kind == FileKind::Other || isTemporaryName(lenient)) {
            return;
        }
        const QByteArray nativePath = folder.nativePath + '/' + name;
        if (!m_visitedFiles.insert(nodeOf(status)).second) {
            return; // a hard link to a file already shared
        }
        if (isDataless(status)) {
            report(displayPath(nativePath), u"an iCloud placeholder: its data isn't on this Mac"_s);
            return;
        }
        const LocalName local = localName(name);
        if (!local.problem.isEmpty()) {
            report(displayPath(nativePath), local.problem);
            return;
        }
        QByteArray virtualName = local.virtualName.toUtf8();
        if (folder.virtualBytes + 1 + virtualName.size() > kMaxVirtualPathBytes) {
            report(displayPath(nativePath), u"its path is too long for peers"_s);
            return;
        }
        if (listing.fileNames.contains(virtualName)) {
            report(displayPath(nativePath), u"another file beside it has the same name for peers"_s);
            return;
        }
        if (m_walk.files >= kMaxFiles || m_poolBytes >= kMaxPoolBytes) {
            markFull(u"more than %1 files to share: the rest aren't shared"_s.arg(m_walk.files));
            return;
        }
        listing.fileNames.insert(virtualName);
        const FileIdentity identity{static_cast<quint64>(status.st_dev), static_cast<quint64>(status.st_ino),
                                    static_cast<quint64>(status.st_size)};
        Attributes attributes;
        if (kind == FileKind::Audio) {
            attributes = m_attributes.audio(folder.keyPath + u'/' + local.key, nativePath, identity, modifiedMs(status));
        }
        m_poolBytes += virtualName.size() + name.size();
        listing.files.push_back({name, std::move(virtualName), identity, attributes});
        ++m_walk.files;
    }

    AttributeSource &m_attributes;
    const audio::CancellationFlag *m_cancel = nullptr;
    std::unordered_set<NodeId, NodeIdHash> m_excludedNodes;
    QStringList m_excludedKeys;
    std::unordered_set<NodeId, NodeIdHash> m_visitedFolders;
    std::unordered_set<NodeId, NodeIdHash> m_visitedFiles;
    QString m_root; // the root being walked, for messages
    qsizetype m_poolBytes = 0;
    bool m_full = false;
    Walk m_walk;
};

} // namespace

Walk walk(const QList<WalkRoot> &roots, const QStringList &excludedFolders, AttributeSource &attributes,
          const audio::CancellationFlag *cancel)
{
    return Walker(excludedFolders, attributes, cancel).run(roots);
}

} // namespace selecta::share::detail
