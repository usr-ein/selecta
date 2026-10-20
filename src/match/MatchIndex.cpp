// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/MatchIndex.h"

#include "match/Duration.h"
#include "match/Quality.h"
#include "match/detail/IndexData.h"
#include "match/detail/Text.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace detail {

QString basenameKey(QStringView path)
{
    const qsizetype slash = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    const QStringView base = slash >= 0 ? path.sliced(slash + 1) : path;
    return base.toString().normalized(QString::NormalizationForm_C).toCaseFolded();
}

QString historySizeNameKey(quint64 size, QStringView basenameKey)
{
    return QString::number(size) + u'␟' + basenameKey;
}

QString historyPeerPathKey(QStringView peer, QStringView path)
{
    return peer.toString() + u'␟' + path;
}

} // namespace detail

namespace {

constexpr qsizetype kMaxHistoryPath = 4096;

QString bounded(const QString &text)
{
    return text.size() > Normalizer::kMaxFieldLength ? text.first(Normalizer::kMaxFieldLength) : text;
}

class IndexBuilder
{
public:
    explicit IndexBuilder(detail::IndexData &data)
        : m_data(data)
    {}

    void addAlbums(const QList<LibraryAlbum> &albums)
    {
        for (const LibraryAlbum &album : albums) {
            if (album.source == Source::History || m_albumIndex.contains(album.id)) {
                ++m_data.dropped;
                continue;
            }
            detail::AlbumRecord record;
            record.id = album.id;
            record.source = album.source;
            const ArtistKeys artist = Normalizer::artistKeys(bounded(album.artist));
            record.artist = artist.field;
            record.artistSet = artist.set;
            record.title = Normalizer::albumKey(bounded(album.title));
            record.year = album.year;
            record.variousArtists = album.variousArtists;
            m_albumIndex.insert(album.id, static_cast<qint32>(m_data.albums.size()));
            m_data.albums.push_back(std::move(record));
        }
    }

    void addTrack(const LibraryTrack &track)
    {
        if (track.source == Source::History ||
            m_data.records.size() >= static_cast<std::size_t>(MatchIndex::kMaxRecords)) {
            ++m_data.dropped;
            return;
        }
        const auto index = static_cast<quint32>(m_data.records.size());
        QString artist = bounded(track.artist).trimmed();
        if (artist.isEmpty()) {
            artist = bounded(track.albumArtist).trimmed(); // no artist tag: the album artist stands in
        }
        const TrackKeys keys = Normalizer::trackKeys(artist, bounded(track.title), bounded(track.album));

        detail::TrackRecord record;
        record.id = track.id;
        record.albumId = track.albumId;
        record.durationMs = std::max<qint64>(track.durationMs, 0);
        record.source = track.source;
        record.quality = static_cast<quint8>(qualityRank(track.quality));
        record.mix = track.durationMs > static_cast<qint64>(kMixMinSec) * 1000;
        record.trackNumber = static_cast<qint16>(std::clamp(track.trackNumber, 0, 9999));
        record.discNumber = static_cast<qint16>(std::clamp(track.discNumber, 0, 999));
        record.album = track.albumId != 0 ? m_albumIndex.value(track.albumId, -1) : -1;
        if (!record.mix) {
            record.titleBegin = static_cast<quint32>(m_data.tokenPool.size());
            record.titleCount = addTokens(m_words, keys.title.field.key, index);
            record.artistBegin = static_cast<quint32>(m_data.tokenPool.size());
            record.artistCount = addTokens(m_words, keys.artist.field.key, index);
            record.titleSquashed = m_squashed.add(squashedTerm(u't', keys.title.field.squashed), index);
            record.artistSquashed = m_squashed.add(squashedTerm(u'a', keys.artist.field.squashed), index);
        }
        record.albumBegin = static_cast<quint32>(m_data.tokenPool.size());
        record.albumCount = addTokens(m_albumWords, keys.album.key, index);
        record.albumSquashed = m_squashed.add(squashedTerm(u'l', keys.album.squashed), index);
        if (keys.title.field.leet != keys.title.field.squashed) {
            m_data.titleLeet.insert(index, keys.title.field.leet);
            record.titleLeet = true;
        }
        if (keys.artist.field.leet != keys.artist.field.squashed) {
            m_data.artistLeet.insert(index, keys.artist.field.leet);
            record.artistLeet = true;
        }
        record.meta = internMeta(keys);
        if (!artist.isEmpty()) {
            int &count = m_artistCounts[artist];
            count = std::min(count, std::numeric_limits<int>::max() - 1) + 1;
        }
        if (record.album >= 0) {
            m_data.albums[static_cast<std::size_t>(record.album)].tracks.push_back(index);
        }
        m_data.records.push_back(record);
    }

    void addHistory(const HistoryEntry &entry)
    {
        if (m_data.history.size() >= static_cast<std::size_t>(MatchIndex::kMaxRecords)) {
            ++m_data.dropped;
            return;
        }
        detail::HistoryRecord record;
        record.id = entry.id;
        record.size = entry.size;
        record.path =
            entry.remotePath.size() > kMaxHistoryPath ? entry.remotePath.last(kMaxHistoryPath) : entry.remotePath;
        record.peer = bounded(entry.peer);
        record.basenameKey = detail::basenameKey(record.path);
        const auto index = static_cast<quint32>(m_data.history.size());
        m_data.historyBySizeName.insert(detail::historySizeNameKey(record.size, record.basenameKey), index);
        if (!record.peer.isEmpty()) {
            m_data.historyByPeerPath.insert(detail::historyPeerPathKey(record.peer, record.path), index);
        }
        m_data.history.push_back(std::move(record));
    }

    void finish()
    {
        finishAlbums();
        m_data.words = m_words.build();
        m_data.albumWords = m_albumWords.build();
        m_data.squashed = m_squashed.build();
        m_data.wordFacts = factsOf(m_data.words);
        m_data.albumWordFacts = factsOf(m_data.albumWords);
        QList<ArtistDictionary::Entry> entries;
        entries.reserve(m_artistCounts.size());
        for (auto it = m_artistCounts.cbegin(); it != m_artistCounts.cend(); ++it) {
            entries.append({it.key(), it.value()});
        }
        m_data.artists = ArtistDictionary(entries);
        m_data.records.shrink_to_fit();
        m_data.tokenPool.shrink_to_fit();
    }

private:
    static std::vector<detail::TokenFacts> factsOf(const TokenIndex &vocabulary)
    {
        std::vector<detail::TokenFacts> facts;
        facts.reserve(static_cast<std::size_t>(vocabulary.termCount()));
        for (qsizetype term = 0; term < vocabulary.termCount(); ++term) {
            facts.push_back(detail::tokenFacts(vocabulary.term(static_cast<quint32>(term))));
        }
        return facts;
    }

    // Squashed keys of the three roles share one exact index, apart by a one-letter role prefix.
    static QString squashedTerm(QChar role, const QString &squashed)
    {
        return squashed.isEmpty() ? QString() : QString(role) + u'␟' + squashed;
    }

    quint16 addTokens(TokenIndex::Builder &builder, const QString &key, quint32 index)
    {
        quint16 count = 0;
        qsizetype start = -1;
        for (qsizetype i = 0; i <= key.size() && count < kMaxFieldTokens; ++i) {
            const bool space = i == key.size() || key.at(i) == u' ';
            if (space && start >= 0) {
                m_data.tokenPool.push_back(builder.add(QStringView(key).sliced(start, i - start), index));
                ++count;
                start = -1;
            } else if (!space && start < 0) {
                start = i;
            }
        }
        return count;
    }

    quint32 internMeta(const TrackKeys &keys)
    {
        const VersionInfo &version = keys.title.version;
        const QString signature = version.key() + u'|' + (version.remaster ? u"r"_s : QString()) +
                                  (version.mixed ? u"m"_s : QString()) + u'|' + version.raw + u'|' +
                                  keys.title.extraKey + u'|' + keys.artist.set.join(u',') + u'|' +
                                  keys.artist.featured.join(u',') + u'|' + keys.title.featured.join(u',');
        if (const auto it = m_metaIds.constFind(signature); it != m_metaIds.cend()) {
            return it.value();
        }
        detail::RecordMeta meta;
        meta.version = version;
        meta.extraKey = keys.title.extraKey;
        meta.artistSet = keys.artist.set;
        meta.featured = keys.artist.featured + keys.title.featured;
        const auto id = static_cast<quint32>(m_data.metas.size());
        m_data.metas.push_back(std::move(meta));
        m_metaIds.insert(signature, id);
        return id;
    }

    void finishAlbums()
    {
        for (std::size_t a = 0; a < m_data.albums.size(); ++a) {
            detail::AlbumRecord &album = m_data.albums[a];
            std::ranges::stable_sort(album.tracks, [this](quint32 x, quint32 y) {
                const detail::TrackRecord &rx = m_data.records[x];
                const detail::TrackRecord &ry = m_data.records[y];
                return std::tie(rx.discNumber, rx.trackNumber) < std::tie(ry.discNumber, ry.trackNumber);
            });
            for (const quint32 track : album.tracks) {
                const qint64 ms = m_data.records[track].durationMs;
                if (ms > 0) {
                    album.sortedDurations.push_back(static_cast<int>(ms / 1000));
                }
            }
            std::ranges::sort(album.sortedDurations);
            const bool complete = !album.tracks.empty() && album.sortedDurations.size() == album.tracks.size();
            if (complete) {
                const int band = album.sortedDurations.front() / detail::kSignatureBandSec;
                m_data.albumsBySignature.insert(detail::signatureKey(static_cast<int>(album.tracks.size()), band),
                                                static_cast<quint32>(a));
            }
        }
    }

    detail::IndexData &m_data;
    TokenIndex::Builder m_words{true};
    TokenIndex::Builder m_albumWords{true};
    TokenIndex::Builder m_squashed{false};
    QHash<QString, quint32> m_metaIds;
    QHash<QString, int> m_artistCounts;
    QHash<quint64, qint32> m_albumIndex;
};

} // namespace

std::shared_ptr<const MatchIndex> MatchIndex::build(const MatchIndexInput &input)
{
    auto data = std::make_unique<detail::IndexData>();
    data->generation = input.generation;
    data->records.reserve(static_cast<std::size_t>(std::min(input.tracks.size(), kMaxRecords)));
    IndexBuilder builder(*data);
    builder.addAlbums(input.albums);
    for (const LibraryTrack &track : input.tracks) {
        builder.addTrack(track);
    }
    for (const HistoryEntry &entry : input.history) {
        builder.addHistory(entry);
    }
    builder.finish();
    return std::shared_ptr<const MatchIndex>(new MatchIndex(std::move(data)));
}

MatchIndex::MatchIndex(std::unique_ptr<detail::IndexData> data)
    : m_data(std::move(data))
{}

MatchIndex::~MatchIndex() = default;

quint64 MatchIndex::generation() const
{
    return m_data->generation;
}

int MatchIndex::normVersion() const
{
    return kNormVersion;
}

qsizetype MatchIndex::trackCount() const
{
    return static_cast<qsizetype>(m_data->records.size());
}

qsizetype MatchIndex::albumCount() const
{
    return static_cast<qsizetype>(m_data->albums.size());
}

qsizetype MatchIndex::historyCount() const
{
    return static_cast<qsizetype>(m_data->history.size());
}

qsizetype MatchIndex::vocabularySize() const
{
    return m_data->words.termCount() + m_data->albumWords.termCount();
}

qsizetype MatchIndex::droppedRecords() const
{
    return m_data->dropped;
}

const ArtistDictionary &MatchIndex::artists() const
{
    return m_data->artists;
}

std::size_t MatchIndex::memoryUsage() const
{
    constexpr std::size_t kHashEntry = 48;   // node and key object, roughly
    constexpr std::size_t kStringBytes = 64; // a short QString's heap block, roughly
    const detail::IndexData &d = *m_data;
    std::size_t bytes = sizeof(detail::IndexData) + d.words.memoryUsage() + d.albumWords.memoryUsage() +
                        d.squashed.memoryUsage() + d.artists.memoryUsage();
    bytes += d.records.capacity() * sizeof(detail::TrackRecord);
    bytes += d.tokenPool.capacity() * sizeof(quint32);
    bytes += (d.wordFacts.capacity() + d.albumWordFacts.capacity()) * sizeof(detail::TokenFacts);
    bytes += d.metas.capacity() * (sizeof(detail::RecordMeta) + (2 * kStringBytes));
    bytes += static_cast<std::size_t>(d.titleLeet.size() + d.artistLeet.size()) * (kHashEntry + kStringBytes);
    for (const detail::AlbumRecord &album : d.albums) {
        bytes += sizeof(detail::AlbumRecord) + (4 * kStringBytes) + (album.tracks.capacity() * sizeof(quint32)) +
                 (album.sortedDurations.capacity() * sizeof(int));
    }
    bytes += static_cast<std::size_t>(d.albumsBySignature.size()) * kHashEntry;
    bytes += d.history.capacity() * (sizeof(detail::HistoryRecord) + (3 * kStringBytes));
    bytes +=
        static_cast<std::size_t>(d.historyBySizeName.size() + d.historyByPeerPath.size()) * (kHashEntry + kStringBytes);
    return bytes;
}

} // namespace selecta::match
