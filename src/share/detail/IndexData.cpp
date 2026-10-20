// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/IndexData.h"

#include <algorithm>
#include <cstring>

namespace selecta::share::detail {

QString extensionOf(QByteArrayView utf8Name)
{
    // '.' is ASCII, so it can be found in the UTF-8 bytes directly.
    const qsizetype dot = utf8Name.lastIndexOf('.');
    if (dot <= 0 || dot + 1 >= utf8Name.size()) {
        return {};
    }
    return QString::fromUtf8(utf8Name.sliced(dot + 1)).toLower();
}

int compareBytes(QByteArrayView a, QByteArrayView b)
{
    const qsizetype common = std::min(a.size(), b.size());
    if (common > 0) {
        if (const int c = std::memcmp(a.data(), b.data(), static_cast<std::size_t>(common)); c != 0) {
            return c;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

int compareReversed(QStringView a, QStringView b)
{
    qsizetype i = a.size();
    qsizetype j = b.size();
    while (i > 0 && j > 0) {
        --i;
        --j;
        if (a[i] != b[j]) {
            return a[i].unicode() < b[j].unicode() ? -1 : 1;
        }
    }
    if (i == 0 && j == 0) {
        return 0;
    }
    return i == 0 ? -1 : 1;
}

QByteArray IndexData::localPath(const FileRecord &file) const
{
    const FolderRecord &folder = folders[file.folder];
    const QByteArrayView name = file.localBytes == 0 ? fileName(file) : bytes(file.name + file.nameBytes, file.localBytes);
    QByteArray path;
    path.reserve(folder.localBytes + 1 + name.size());
    path.append(bytes(folder.local, folder.localBytes));
    path.append('/');
    path.append(name);
    return path;
}

slsk::FileEntry IndexData::entry(quint32 id, bool fullPath) const
{
    const FileRecord &file = files[id];
    const QByteArrayView name = fileName(file);
    slsk::FileEntry entry;
    if (fullPath) {
        const QByteArrayView folder = folderPath(folders[file.folder]);
        entry.name.raw.reserve(folder.size() + 1 + name.size());
        entry.name.raw.append(folder);
        entry.name.raw.append('\\');
        entry.name.raw.append(name);
    } else {
        entry.name.raw = name.toByteArray();
    }
    entry.size = file.size;
    entry.extension = extensionOf(name);
    entry.attributes = file.attributes.toWire();
    return entry;
}

qint64 IndexData::findFolder(QStringView virtualPath) const
{
    if (const auto it = folderByPath.constFind(virtualPath.toUtf8()); it != folderByPath.cend()) {
        return it.value();
    }
    const QString folded = virtualPath.toString().toCaseFolded();
    qint64 first = -1;
    for (auto it = folderByFoldedPath.constFind(folded); it != folderByFoldedPath.cend() && it.key() == folded; ++it) {
        if (first < 0 || it.value() < first) {
            first = it.value();
        }
    }
    return first;
}

qsizetype IndexData::measure() const
{
    // Per hash entry: the node, its share of the bucket array and the key's array header.
    constexpr qsizetype kEntryOverhead = 48;
    qsizetype total = pool.capacity() + sharedFileList.capacity();
    total += static_cast<qsizetype>((folders.capacity() * sizeof(FolderRecord)) + (files.capacity() * sizeof(FileRecord)));
    total += static_cast<qsizetype>(wordChars.capacity() * sizeof(char16_t));
    total += static_cast<qsizetype>((wordStart.capacity() + postingStart.capacity() + postings.capacity() +
                                     byReversed.capacity()) *
                                    sizeof(quint32));
    for (auto it = folderByPath.cbegin(); it != folderByPath.cend(); ++it) {
        total += kEntryOverhead + it.key().capacity();
    }
    for (auto it = folderByFoldedPath.cbegin(); it != folderByFoldedPath.cend(); ++it) {
        total += kEntryOverhead + (it.key().capacity() * 2);
    }
    for (const ShareRoot &root : roots) {
        total += kEntryOverhead + ((root.path.capacity() + root.alias.capacity()) * 2);
    }
    return total;
}

} // namespace selecta::share::detail
