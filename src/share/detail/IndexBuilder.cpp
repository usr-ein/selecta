// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/IndexBuilder.h"

#include "share/detail/Text.h"
#include "slsk/PeerMessages.h"

#include <QHash>

#include <algorithm>
#include <numeric>
#include <utility>

namespace selecta::share::detail {

namespace {

// How often the slow loops look at the cancellation flag.
constexpr std::size_t kCancelCheckEvery = 4096;

void sortUnique(std::vector<quint32> &ids)
{
    std::ranges::sort(ids);
    const auto [first, last] = std::ranges::unique(ids);
    ids.erase(first, last);
}

class Builder
{
public:
    Builder(Walk walk, const QList<ShareRoot> &roots, const audio::CancellationFlag *cancel)
        : m_walk(std::move(walk))
        , m_cancel(cancel)
        , m_data(std::make_unique<IndexData>())
    {
        m_data->roots = roots;
    }

    std::unique_ptr<const IndexData> run()
    {
        layOut();
        if (cancelled() || !indexWords() || cancelled()) {
            return nullptr;
        }
        indexFolders();
        encodeList();
        m_data->counts = {static_cast<int>(m_data->folders.size()), static_cast<int>(m_data->files.size())};
        m_data->memoryBytes = m_data->measure();
        return std::move(m_data);
    }

private:
    bool cancelled() const { return m_cancel != nullptr && m_cancel->isCancelled(); }

    quint32 append(QByteArrayView bytes)
    {
        // The walk stops at kMaxPoolBytes, well inside 32-bit offsets.
        const auto offset = static_cast<quint32>(m_data->pool.size());
        m_data->pool.append(bytes);
        return offset;
    }

    // Records and strings, in walk order; folders whose subtree holds no file are left out.
    void layOut()
    {
        const std::vector<WalkedFolder> &folders = m_walk.folders;
        std::vector<qsizetype> subtree(folders.size(), 0);
        for (std::size_t i = folders.size(); i-- > 0;) {
            subtree[i] += static_cast<qsizetype>(folders[i].files.size());
            if (folders[i].parent >= 0) {
                subtree[static_cast<std::size_t>(folders[i].parent)] += subtree[i];
            }
        }
        m_newId.assign(folders.size(), -1);
        qsizetype poolBytes = 0;
        for (std::size_t i = 0; i < folders.size(); ++i) {
            if (subtree[i] > 0) {
                m_newId[i] = static_cast<qint64>(m_kept.size());
                m_kept.push_back(i);
                poolBytes += folders[i].nativePath.size() + (folders[i].virtualPath.size() * 2);
                for (const WalkedFile &file : folders[i].files) {
                    poolBytes += file.virtualName.size() + file.nativeName.size();
                }
            }
        }
        m_data->pool.reserve(poolBytes);
        m_data->folders.reserve(m_kept.size());
        m_data->files.reserve(static_cast<std::size_t>(m_walk.files));
        for (const std::size_t i : m_kept) {
            const WalkedFolder &walked = folders[i];
            FolderRecord record;
            const QByteArray path = walked.virtualPath.toUtf8();
            record.path = append(path);
            record.pathBytes = static_cast<quint32>(path.size());
            record.local = append(walked.nativePath);
            record.localBytes = static_cast<quint32>(walked.nativePath.size());
            record.firstFile = static_cast<quint32>(m_data->files.size());
            record.fileCount = static_cast<quint32>(walked.files.size());
            for (const WalkedFile &file : walked.files) {
                FileRecord entry;
                entry.size = file.identity.size;
                entry.device = file.identity.device;
                entry.inode = file.identity.inode;
                entry.folder = static_cast<quint32>(m_data->folders.size());
                entry.name = append(file.virtualName);
                entry.nameBytes = static_cast<quint16>(file.virtualName.size());
                if (file.nativeName != file.virtualName) { // NFD on disk, or a `\` in the name
                    append(file.nativeName);
                    entry.localBytes = static_cast<quint16>(file.nativeName.size());
                }
                entry.attributes = file.attributes;
                m_data->files.push_back(entry);
            }
            m_data->folders.push_back(record);
        }
    }

    quint32 wordId(const QString &word)
    {
        const auto it = m_wordIds.constFind(word);
        if (it != m_wordIds.cend()) {
            return it.value();
        }
        const auto id = static_cast<quint32>(m_words.size());
        m_words.append(word);
        m_wordIds.insert(word, id);
        return id;
    }

    // The dictionary, the postings and the reversed order. False when cancelled.
    bool indexWords()
    {
        // Each folder's words: its own name's and its parent's (the whole virtual path, alias included).
        std::vector<std::vector<quint32>> folderWords(m_kept.size());
        for (std::size_t k = 0; k < m_kept.size(); ++k) {
            const WalkedFolder &walked = m_walk.folders[m_kept[k]];
            std::vector<quint32> words;
            if (walked.parent >= 0) {
                words = folderWords[static_cast<std::size_t>(m_newId[static_cast<std::size_t>(walked.parent)])];
            }
            const QStringList own = searchWords(walked.segment);
            for (const QString &word : own) {
                words.push_back(wordId(word));
            }
            sortUnique(words);
            folderWords[k] = std::move(words);
        }
        // (word, file) for every distinct word of every file, in file order: each word's files come out ascending.
        std::vector<std::pair<quint32, quint32>> pairs;
        pairs.reserve(m_data->files.size() * 12);
        std::vector<quint32> words;
        for (std::size_t f = 0; f < m_data->files.size(); ++f) {
            if (f % kCancelCheckEvery == 0 && cancelled()) {
                return false;
            }
            const FileRecord &file = m_data->files[f];
            words = folderWords[file.folder];
            const QStringList own = searchWords(QString::fromUtf8(m_data->fileName(file)));
            for (const QString &word : own) {
                words.push_back(wordId(word));
            }
            sortUnique(words);
            for (const quint32 word : words) {
                pairs.emplace_back(word, static_cast<quint32>(f));
            }
        }
        // Word ids become ranks in the sorted dictionary.
        const std::size_t count = static_cast<std::size_t>(m_words.size());
        std::vector<quint32> order(count);
        std::iota(order.begin(), order.end(), 0U);
        std::ranges::sort(order, [this](quint32 a, quint32 b) {
            return QStringView(m_words.at(a)).compare(QStringView(m_words.at(b))) < 0;
        });
        std::vector<quint32> rank(count);
        m_data->wordStart.reserve(count + 1);
        m_data->wordStart.push_back(0);
        for (std::size_t r = 0; r < count; ++r) {
            rank[order[r]] = static_cast<quint32>(r);
            const QString &word = m_words.at(order[r]);
            m_data->wordChars.insert(m_data->wordChars.end(), word.utf16(), word.utf16() + word.size());
            m_data->wordStart.push_back(static_cast<quint32>(m_data->wordChars.size()));
        }
        m_wordIds.clear();
        m_words.clear();
        std::vector<quint32> &start = m_data->postingStart;
        start.assign(count + 1, 0);
        for (const auto &[word, file] : pairs) {
            ++start[rank[word] + 1];
        }
        std::partial_sum(start.begin(), start.end(), start.begin());
        m_data->postings.resize(pairs.size());
        std::vector<quint32> cursor(start.begin(), start.end() - 1);
        for (const auto &[word, file] : pairs) {
            m_data->postings[cursor[rank[word]]++] = file;
        }
        m_data->byReversed.resize(count);
        std::iota(m_data->byReversed.begin(), m_data->byReversed.end(), 0U);
        std::ranges::sort(m_data->byReversed, [this](quint32 a, quint32 b) {
            return compareReversed(m_data->word(a), m_data->word(b)) < 0;
        });
        return true;
    }

    void indexFolders()
    {
        m_data->folderByPath.reserve(static_cast<qsizetype>(m_data->folders.size()));
        m_data->folderByFoldedPath.reserve(static_cast<qsizetype>(m_data->folders.size()));
        for (std::size_t k = 0; k < m_data->folders.size(); ++k) {
            const QByteArrayView path = m_data->folderPath(m_data->folders[k]);
            m_data->folderByPath.insert(path.toByteArray(), static_cast<quint32>(k));
            m_data->folderByFoldedPath.insert(QString::fromUtf8(path).toCaseFolded(), static_cast<quint32>(k));
        }
    }

    void encodeList()
    {
        slsk::peer::SharedFileListResponse response;
        response.list.directories.reserve(static_cast<qsizetype>(m_data->folders.size()));
        for (const FolderRecord &folder : m_data->folders) {
            slsk::SharedDirectory directory;
            directory.path.raw = m_data->folderPath(folder).toByteArray();
            directory.files.reserve(folder.fileCount);
            for (quint32 f = folder.firstFile; f < folder.firstFile + folder.fileCount; ++f) {
                directory.files.append(m_data->entry(f, false));
            }
            response.list.directories.append(std::move(directory));
        }
        m_data->sharedFileList = response.encode();
    }

    Walk m_walk;
    const audio::CancellationFlag *m_cancel = nullptr;
    std::unique_ptr<IndexData> m_data;
    std::vector<std::size_t> m_kept; // walk indexes of the folders kept, in order
    std::vector<qint64> m_newId;     // walk index → folder id, -1 when left out
    QHash<QString, quint32> m_wordIds;
    QStringList m_words;
};

} // namespace

std::shared_ptr<const ShareIndex> IndexBuilder::build(Walk walk, const QList<ShareRoot> &roots,
                                                      const audio::CancellationFlag *cancel)
{
    std::unique_ptr<const IndexData> data = Builder(std::move(walk), roots, cancel).run();
    if (!data) {
        return nullptr;
    }
    return wrap(std::move(data));
}

std::unique_ptr<const IndexData> IndexBuilder::emptyData(const QList<ShareRoot> &roots)
{
    auto data = std::make_unique<IndexData>();
    data->roots = roots;
    data->sharedFileList = slsk::peer::SharedFileListResponse{}.encode();
    data->memoryBytes = data->measure();
    return data;
}

std::shared_ptr<const ShareIndex> IndexBuilder::empty(const QList<ShareRoot> &roots)
{
    return wrap(emptyData(roots));
}

std::shared_ptr<const ShareIndex> IndexBuilder::wrap(std::unique_ptr<const IndexData> data)
{
    // The constructor is private to the builder: std::make_shared can't reach it.
    return std::shared_ptr<const ShareIndex>(new ShareIndex(std::move(data))); // NOLINT(modernize-make-shared)
}

} // namespace selecta::share::detail
