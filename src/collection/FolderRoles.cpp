// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/FolderRoles.h"

#include "collection/detail/Walk.h"

#include <QDir>

using namespace Qt::StringLiterals;

namespace selecta::collection {

namespace {

// The folder's cleaned absolute path, or an empty string for a folder not chosen (empty or relative).
QString chosen(const QString &folder)
{
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder)) {
        return {};
    }
    return QDir::cleanPath(folder);
}

QString cleanAbsolute(const QString &path)
{
    return QDir::cleanPath(QDir::isAbsolutePath(path) ? path : QDir::current().absoluteFilePath(path));
}

QStringView parentOf(QStringView key)
{
    const qsizetype slash = key.lastIndexOf(u'/');
    return slash <= 0 ? QStringView(u"/") : key.left(slash);
}

} // namespace

FolderRoles::FolderRoles(const QString &tracks, const QString &albums)
    : m_tracks(chosen(tracks))
    , m_albums(chosen(albums))
    , m_tracksKey(m_tracks.isEmpty() ? QString() : comparisonKey(m_tracks))
    , m_albumsKey(m_albums.isEmpty() ? QString() : comparisonKey(m_albums))
{}

QStringList FolderRoles::roots() const
{
    QStringList roots;
    if (!m_tracks.isEmpty()) {
        roots.append(m_tracks);
    }
    if (!m_albums.isEmpty() && m_albumsKey != m_tracksKey) {
        roots.append(m_albums);
    }
    return roots;
}

void FolderRoles::applyTo(ScanOptions &options) const
{
    options.tracksFolders = m_tracks.isEmpty() ? QStringList() : QStringList{m_tracks};
    options.albumsFolders = m_albums.isEmpty() ? QStringList() : QStringList{m_albums};
}

const QString &FolderRoles::nearestRoot(const QString &key) const
{
    static const QString kNone;
    const bool inTracks = !m_tracksKey.isEmpty() && detail::isUnder(key, m_tracksKey);
    const bool inAlbums = !m_albumsKey.isEmpty() && detail::isUnder(key, m_albumsKey);
    if (inTracks && inAlbums) {
        return m_tracksKey.size() >= m_albumsKey.size() ? m_tracksKey : m_albumsKey; // the deeper one
    }
    if (inTracks) {
        return m_tracksKey;
    }
    return inAlbums ? m_albumsKey : kNone;
}

QStringList FolderRoles::rescanFor(const QString &folder) const
{
    const QString key = comparisonKey(folder);
    const QString &nearest = nearestRoot(key);
    if (nearest.isEmpty()) {
        return {};
    }
    QStringList folders;
    // In the tracks folder only its own files count: a change in one of its subfolders concerns nobody.
    if (nearest == m_albumsKey || key == m_tracksKey) {
        folders.append(cleanAbsolute(folder));
    }
    // A collection folder inside `folder` has its own role, which a scan of `folder` wouldn't apply (the tracks folder
    // doesn't descend into an albums folder inside it): it is rescanned as a root of its own.
    const auto addInside = [&](const QString &root, const QString &rootKey) {
        if (!rootKey.isEmpty() && rootKey != key && detail::isUnder(rootKey, key) && !folders.contains(root)) {
            folders.append(root);
        }
    };
    addInside(m_tracks, m_tracksKey);
    addInside(m_albums, m_albumsKey);
    return folders;
}

FolderRoles::Role FolderRoles::roleOf(const QString &path, QString *albumFolder) const
{
    const QString key = comparisonKey(path);
    const QString &nearest = nearestRoot(key);
    if (nearest.isEmpty() || key == nearest) {
        return Role::None;
    }
    const QStringView parent = parentOf(key);
    if (nearest == m_tracksKey && parent == m_tracksKey) {
        return Role::Track; // lying in the tracks folder (also when it is the albums folder too)
    }
    if (nearest != m_albumsKey || parent == m_albumsKey) {
        return Role::None; // in a subfolder of the tracks folder, or lying in the albums folder
    }
    if (albumFolder != nullptr) {
        // The subfolder of the albums folder that holds the file: one component more than the albums folder. The key
        // and the path differ only in their normalisation, never in their slashes.
        const qsizetype albumsParts = m_albumsKey == u"/"_s ? 1 : m_albumsKey.count(u'/') + 1;
        *albumFolder = cleanAbsolute(path).split(u'/').mid(0, albumsParts + 1).join(u'/');
    }
    return Role::Album;
}

} // namespace selecta::collection
