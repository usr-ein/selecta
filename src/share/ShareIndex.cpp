// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/ShareIndex.h"

#include "share/detail/Attributes.h"
#include "share/detail/IndexBuilder.h"
#include "share/detail/IndexData.h"
#include "share/detail/Query.h"
#include "share/detail/Text.h"
#include "share/detail/Walk.h"

#include <QAnyStringView>
#include <QDir>
#include <QUtf8StringView>

#include <algorithm>
#include <span>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::share {

namespace {

using detail::IndexData;

// Walks one posting list forward: callers ask about files in ascending order, so each list is crossed once, in
// galloping steps (cheap when it is much longer than the list that drives the query).
class Cursor
{
public:
    explicit Cursor(std::span<const quint32> list)
        : m_list(list)
    {}

    bool contains(quint32 file)
    {
        const std::size_t size = m_list.size();
        if (m_at < size && m_list[m_at] < file) {
            std::size_t low = m_at; // m_list[low] < file
            std::size_t high = m_at + 1;
            std::size_t step = 1;
            while (high < size && m_list[high] < file) {
                low = high;
                step *= 2;
                high = low + step;
            }
            high = std::min(high, size);
            const auto from = m_list.begin() + static_cast<std::ptrdiff_t>(low + 1);
            const auto to = m_list.begin() + static_cast<std::ptrdiff_t>(high);
            m_at = static_cast<std::size_t>(std::lower_bound(from, to, file) - m_list.begin());
        }
        return m_at < size && m_list[m_at] == file;
    }

private:
    std::span<const quint32> m_list;
    std::size_t m_at = 0;
};

// The id of a whole word, by binary search in the sorted dictionary.
std::optional<quint32> findWord(const IndexData &d, QStringView word)
{
    const std::size_t count = d.wordCount();
    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + ((high - low) / 2);
        if (d.word(static_cast<quint32>(middle)).compare(word) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < count && d.word(static_cast<quint32>(low)).compare(word) == 0) {
        return static_cast<quint32>(low);
    }
    return std::nullopt;
}

// The files with a word ending with `suffix`, as a bitmap over file ids. Empty when no word ends so.
std::vector<quint64> filesEndingWith(const IndexData &d, QStringView suffix)
{
    const auto first = std::ranges::lower_bound(
        d.byReversed, suffix, [](QStringView a, QStringView b) { return detail::compareReversed(a, b) < 0; },
        [&d](quint32 id) { return d.word(id); });
    const auto last = std::partition_point(first, d.byReversed.end(),
                                           [&d, suffix](quint32 id) { return d.word(id).endsWith(suffix); });
    if (first == last) {
        return {};
    }
    std::vector<quint64> bits((d.files.size() + 63) / 64, 0);
    for (auto it = first; it != last; ++it) {
        for (const quint32 file : d.postingsOf(*it)) {
            bits[file >> 6U] |= quint64{1} << (file & 63U);
        }
    }
    return bits;
}

bool hasBit(const std::vector<quint64> &bits, quint32 file)
{
    return (bits[file >> 6U] & (quint64{1} << (file & 63U))) != 0;
}

bool containsPhrase(const QByteArray &path, const QStringList &phrases)
{
    const QString text = QString::fromUtf8(path);
    return std::ranges::any_of(phrases,
                               [&text](const QString &phrase) { return text.contains(phrase, Qt::CaseInsensitive); });
}

QList<slsk::FileEntry> evaluate(const IndexData &d, const detail::ParsedQuery &query, const QStringList &phrases,
                                int cap)
{
    std::vector<std::span<const quint32>> plain;
    for (const QString &word : query.plain) {
        const std::optional<quint32> id = findWord(d, word);
        if (!id) {
            return {}; // a word no file has
        }
        plain.push_back(d.postingsOf(*id));
    }
    std::ranges::sort(plain, {}, [](std::span<const quint32> list) { return list.size(); });
    std::vector<std::vector<quint64>> partial;
    for (const QString &suffix : query.partial) {
        partial.push_back(filesEndingWith(d, suffix));
        if (partial.back().empty()) {
            return {};
        }
    }
    std::vector<Cursor> others;
    for (std::size_t i = 1; i < plain.size(); ++i) {
        others.emplace_back(plain[i]);
    }
    std::vector<Cursor> excluded;
    for (const QString &word : query.excluded) {
        if (const std::optional<quint32> id = findWord(d, word)) {
            excluded.emplace_back(d.postingsOf(*id));
        }
    }
    QList<slsk::FileEntry> results;
    // The shortest list drives; the plain words guarantee there is one.
    for (const quint32 file : plain.front()) {
        if (!std::ranges::all_of(others, [file](Cursor &c) { return c.contains(file); }) ||
            !std::ranges::all_of(partial, [file](const auto &bits) { return hasBit(bits, file); }) ||
            std::ranges::any_of(excluded, [file](Cursor &c) { return c.contains(file); })) {
            continue;
        }
        slsk::FileEntry entry = d.entry(file, true);
        if (!phrases.isEmpty() && containsPhrase(entry.name.raw, phrases)) {
            continue;
        }
        results.append(std::move(entry));
        if (results.size() >= cap) {
            break;
        }
    }
    return results;
}

// A peer's path as the index spells paths: decoded as RemotePath::display() does, NFC. Empty when too long.
QString requestText(const slsk::RemotePath &path)
{
    if (path.raw.isEmpty() || path.raw.size() > kMaxRequestPathBytes) {
        return {};
    }
    return path.display().normalized(QString::NormalizationForm_C);
}

// The file `name` in folder `folder`: by binary search for the exact name, else -1.
qint64 findExactName(const IndexData &d, quint32 folder, QByteArrayView name)
{
    const detail::FolderRecord &record = d.folders[folder];
    const auto begin = d.files.begin() + record.firstFile;
    const auto end = begin + record.fileCount;
    const auto it = std::lower_bound(begin, end, name, [&d](const detail::FileRecord &file, QByteArrayView n) {
        return detail::compareBytes(d.fileName(file), n) < 0;
    });
    if (it != end && detail::compareBytes(d.fileName(*it), name) == 0) {
        return it - d.files.begin();
    }
    return -1;
}

// The file at `folderPath`\`name`: exactly, then ignoring case, the first in index order. -1 when none.
qint64 findFile(const IndexData &d, QStringView folderPath, QStringView name)
{
    if (const auto it = d.folderByPath.constFind(folderPath.toUtf8()); it != d.folderByPath.cend()) {
        if (const qint64 file = findExactName(d, it.value(), name.toUtf8()); file >= 0) {
            return file;
        }
    }
    const QString folded = folderPath.toString().toCaseFolded();
    std::vector<quint32> folders;
    for (auto it = d.folderByFoldedPath.constFind(folded); it != d.folderByFoldedPath.cend() && it.key() == folded;
         ++it) {
        folders.push_back(it.value());
    }
    std::ranges::sort(folders);
    for (const quint32 folder : folders) {
        const detail::FolderRecord &record = d.folders[folder];
        for (quint32 f = record.firstFile; f < record.firstFile + record.fileCount; ++f) {
            const QByteArrayView candidate = d.fileName(d.files[f]);
            if (QAnyStringView::compare(QUtf8StringView(candidate.data(), candidate.size()), name,
                                        Qt::CaseInsensitive) == 0) {
                return f;
            }
        }
    }
    return -1;
}

} // namespace

ShareIndex::ShareIndex()
    : d(detail::IndexBuilder::emptyData({}))
{}

ShareIndex::ShareIndex(std::unique_ptr<const detail::IndexData> data)
    : d(std::move(data))
{}

ShareIndex::~ShareIndex() = default;

QByteArray ShareIndex::sharedFileList() const
{
    return d->sharedFileList;
}

QList<slsk::SharedDirectory> ShareIndex::folderContents(const slsk::RemotePath &folder) const
{
    const QString text = requestText(folder);
    if (text.isEmpty()) {
        return {};
    }
    const qint64 found = d->findFolder(text);
    if (found < 0) {
        return {};
    }
    const detail::FolderRecord &record = d->folders[static_cast<std::size_t>(found)];
    slsk::SharedDirectory directory;
    directory.path.raw = d->folderPath(record).toByteArray();
    directory.files.reserve(record.fileCount);
    for (quint32 f = record.firstFile; f < record.firstFile + record.fileCount; ++f) {
        directory.files.append(d->entry(f, false));
    }
    return {directory};
}

QList<slsk::FileEntry> ShareIndex::search(QStringView query, const QStringList &excludedPhrases, int cap) const
{
    if (cap <= 0 || query.size() > kMaxQueryChars || d->files.empty()) {
        return {};
    }
    QStringList phrases;
    for (const QString &phrase : excludedPhrases) {
        if (phrase.isEmpty()) {
            continue; // an empty phrase would silence every answer
        }
        if (query.contains(phrase, Qt::CaseInsensitive)) {
            return {};
        }
        phrases.append(phrase);
    }
    const detail::ParsedQuery parsed = detail::parseQuery(query);
    if (!parsed.isAnswerable()) {
        return {};
    }
    return evaluate(*d, parsed, phrases, std::min(cap, kMaxSearchCap));
}

std::optional<SharedFile> ShareIndex::lookup(const slsk::RemotePath &path) const
{
    const QString text = requestText(path);
    const qsizetype cut = text.lastIndexOf(u'\\');
    if (cut <= 0 || cut + 1 >= text.size()) {
        return std::nullopt;
    }
    const qint64 found = findFile(*d, QStringView(text).first(cut), QStringView(text).sliced(cut + 1));
    if (found < 0) {
        return std::nullopt;
    }
    const detail::FileRecord &file = d->files[static_cast<std::size_t>(found)];
    return SharedFile{QString::fromUtf8(d->localPath(file)), file.size, file.device, file.inode,
                      d->entry(static_cast<quint32>(found), true)};
}

ShareIndex::Counts ShareIndex::counts() const
{
    return d->counts;
}

QList<ShareRoot> ShareIndex::roots() const
{
    return d->roots;
}

qsizetype ShareIndex::memoryBytes() const
{
    return d->memoryBytes;
}

BuildResult build(const QList<ShareRoot> &roots, const QHash<QString, collection::ScannedFile> &knownAudio,
                  const BuildOptions &options, const audio::CancellationFlag *cancel)
{
    const detail::MaterializationOff materialization;
    QStringList aliases;
    aliases.reserve(roots.size());
    for (const ShareRoot &root : roots) {
        aliases.append(root.alias);
    }
    const QStringList unique = detail::uniqueAliases(aliases);
    BuildResult result;
    QList<ShareRoot> built;
    QList<detail::WalkRoot> walked;
    for (qsizetype i = 0; i < roots.size(); ++i) {
        const QString &path = roots.at(i).path;
        // A relative path would be resolved against whatever folder the process runs in: never share that.
        if (!QDir::isAbsolutePath(path)) {
            built.append({path, unique.at(i)});
            result.unreadable.append({path, u"not an absolute path"_s});
            continue;
        }
        const QString cleaned = QDir::cleanPath(path);
        built.append({cleaned, unique.at(i)});
        walked.append({cleaned, unique.at(i)});
    }
    detail::AttributeSource attributes(knownAudio);
    detail::Walk walk = detail::walk(walked, options.excludedFolders, attributes, cancel);
    result.unreadable.append(walk.unreadable);
    if (!walk.cancelled) {
        result.index = detail::IndexBuilder::build(std::move(walk), built, cancel);
    }
    if (!result.index) {
        result.cancelled = true;
        result.index = detail::IndexBuilder::empty(built);
    }
    return result;
}

} // namespace selecta::share
