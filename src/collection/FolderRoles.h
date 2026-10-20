// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"

#include <QString>
#include <QStringList>

namespace selecta::collection {

// The collection's two folders and what each holds (PRD LIB-1):
// - the tracks folder holds loose tracks: the files lying in it belong to the collection, its subfolders don't;
// - the albums folder holds one album per subfolder: every file under a subfolder (disc folders included) is a track
//   of that album, whatever its tags say; files lying in the albums folder itself don't belong to the collection.
// Either folder may be unset (not chosen yet). Settings keeps the two apart (core::checkLibraryConfig), but every
// layout still scans sensibly: one folder for both holds loose tracks and album folders side by side; a tracks folder
// inside the albums folder is still a tracks folder, never an album; an albums folder inside the tracks folder still
// holds albums.
//
// A value type. Pure; any thread. Paths are cleaned and absolute; comparisons use comparisonKey() (NFC).
class FolderRoles
{
public:
    FolderRoles() = default;
    // Absolute folders, or empty strings for folders not chosen yet; relative paths count as not chosen.
    FolderRoles(const QString &tracks, const QString &albums);

    const QString &tracks() const { return m_tracks; }
    const QString &albums() const { return m_albums; }
    bool isEmpty() const { return m_tracks.isEmpty() && m_albums.isEmpty(); }

    // The folders to scan and watch, each once: the tracks folder, then the albums folder.
    QStringList roots() const;
    // Sets options.tracksFolders and options.albumsFolders: what scan() needs to apply these roles.
    void applyTo(ScanOptions &options) const;
    // The folders to rescan after a change in `folder` (a Watcher report). Nothing when the change can't concern the
    // collection: outside both folders, or in a subfolder of the tracks folder. Otherwise `folder` itself, with the
    // collection folders that lie inside it, which a scan of `folder` alone wouldn't reach under the roles.
    QStringList rescanFor(const QString &folder) const;

    enum class Role {
        None,  // not part of the collection
        Track, // a loose track, lying in the tracks folder
        Album, // a track of an album: under a subfolder of the albums folder
    };
    // What the file at `path` is. For Role::Album, `albumFolder` (when given) receives the album's folder: the
    // subfolder of the albums folder that holds the file, spelled as in `path`.
    Role roleOf(const QString &path, QString *albumFolder = nullptr) const;

    friend bool operator==(const FolderRoles &a, const FolderRoles &b)
    {
        return a.m_tracksKey == b.m_tracksKey && a.m_albumsKey == b.m_albumsKey;
    }

private:
    // The deepest collection folder holding `key` (a comparison key), as itself or below it: its key, or empty.
    const QString &nearestRoot(const QString &key) const;

    QString m_tracks;
    QString m_albums;
    QString m_tracksKey; // comparisonKey(m_tracks); empty when unset
    QString m_albumsKey;
};

} // namespace selecta::collection
