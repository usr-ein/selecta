// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "share/ShareIndex.h"
#include "share/detail/Attributes.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QMultiHash>
#include <QString>
#include <QStringView>

#include <span>
#include <vector>

// Internal: the arrays behind a ShareIndex. Immutable once built; read from any number of threads.
namespace selecta::share::detail {

// Who may see a folder. v1 shares everything publicly (decision D-S4); P1's buddy-only folders add a level here.
enum class AccessLevel : quint8 {
    Public,
};

struct FolderRecord
{
    quint32 path = 0; // offset in IndexData::pool of the virtual path (UTF-8, NFC)
    quint32 pathBytes = 0;
    quint32 local = 0; // offset of the local path, the file system's bytes
    quint32 localBytes = 0;
    quint32 firstFile = 0; // the folder's files are [firstFile, firstFile + fileCount), sorted by virtual name bytes
    quint32 fileCount = 0;
    AccessLevel level = AccessLevel::Public;
};

struct FileRecord
{
    quint64 size = 0;
    quint64 device = 0;
    quint64 inode = 0;
    quint32 folder = 0;
    quint32 name = 0; // offset of the virtual name (UTF-8, NFC)
    quint16 nameBytes = 0;
    quint16 localBytes = 0; // 0: the local name is the virtual name; otherwise it follows it in the pool
    Attributes attributes;
};

struct IndexData
{
    QList<ShareRoot> roots;
    QByteArray pool; // every string of the records
    std::vector<FolderRecord> folders;
    std::vector<FileRecord> files;

    // The dictionary, sorted (a word's id is its rank): word w is wordChars[wordStart[w], wordStart[w + 1]).
    std::vector<char16_t> wordChars;
    std::vector<quint32> wordStart;
    // The files with word w: postings[postingStart[w], postingStart[w + 1]), ascending.
    std::vector<quint32> postingStart;
    std::vector<quint32> postings;
    // Word ids sorted by their spelling read backwards: the words ending with a suffix are one range.
    std::vector<quint32> byReversed;

    QHash<QByteArray, quint32> folderByPath;         // exact virtual path (UTF-8) → folder
    QMultiHash<QString, quint32> folderByFoldedPath; // case-folded virtual path → folders

    QByteArray sharedFileList; // the precompressed SharedFileListResponse payload
    ShareIndex::Counts counts;
    qsizetype memoryBytes = 0; // measured once built

    QByteArrayView bytes(quint32 offset, quint32 size) const { return {pool.constData() + offset, size}; }
    QByteArrayView folderPath(const FolderRecord &folder) const { return bytes(folder.path, folder.pathBytes); }
    QByteArrayView fileName(const FileRecord &file) const { return bytes(file.name, file.nameBytes); }
    // The file's local path, the file system's bytes.
    QByteArray localPath(const FileRecord &file) const;

    std::size_t wordCount() const { return wordStart.empty() ? 0 : wordStart.size() - 1; }
    QStringView word(quint32 id) const
    {
        return {wordChars.data() + wordStart[id], static_cast<qsizetype>(wordStart[id + 1] - wordStart[id])};
    }
    std::span<const quint32> postingsOf(quint32 word) const
    {
        return std::span<const quint32>(postings).subspan(postingStart[word],
                                                          postingStart[word + 1] - postingStart[word]);
    }

    // A file as answers carry it: the full virtual path (search answers, lookups) or the bare name (listings).
    slsk::FileEntry entry(quint32 id, bool fullPath) const;
    // The folder with this virtual path: exactly, then ignoring case (the first in index order). -1 when none.
    qint64 findFolder(QStringView virtualPath) const;
    // The approximate heap bytes of everything above.
    qsizetype measure() const;
};

// The extension of a virtual name (UTF-8), lower-cased, as detail::lowerExtension() gives it.
QString extensionOf(QByteArrayView utf8Name);
// memcmp order, the order of a folder's files.
int compareBytes(QByteArrayView a, QByteArrayView b);
// The order of words read backwards, code unit by code unit (a word that runs out first sorts first): the order of
// IndexData::byReversed.
int compareReversed(QStringView a, QStringView b);

} // namespace selecta::share::detail
