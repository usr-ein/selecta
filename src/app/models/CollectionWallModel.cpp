// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/CollectionWallModel.h"

#include "app/Format.h"
#include "match/Quality.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using core::CollectionTrack;
using core::QualityTier;

QString countOfTracks(int count)
{
    return count == 1 ? CollectionWallModel::tr("1 track") : CollectionWallModel::tr("%1 tracks").arg(count);
}

// What a track is called: its title, else its file's name.
QString titleOf(const CollectionTrack &track)
{
    return track.title.isEmpty() ? QFileInfo(track.path).completeBaseName() : track.title;
}

// What an album is called: its title, else its folder's name.
QString albumTitle(const core::CollectionAlbum &album)
{
    return album.title.isEmpty() ? QFileInfo(album.folder).fileName() : album.title;
}

// A loose track's tooltip: names, album, quality and length, genre, file.
QString trackToolTip(const CollectionTrack &track)
{
    QStringList lines{track.artist.isEmpty() ? titleOf(track)
                                             : CollectionWallModel::tr("%1 — %2").arg(track.artist, titleOf(track))};
    if (!track.album.isEmpty()) {
        lines.append(track.year > 0 ? CollectionWallModel::tr("%1 (%2)").arg(track.album).arg(track.year)
                                    : track.album);
    }
    QString quality = CollectionWallModel::qualityText(track);
    if (track.durationMs > 0) {
        quality += u" · "_s + format::length(track.durationMs);
    }
    lines.append(quality);
    if (!track.genre.isEmpty()) {
        lines.append(track.genre);
    }
    lines.append(QDir::toNativeSeparators(track.path));
    return lines.join(u'\n');
}

} // namespace

CollectionWallModel::CollectionWallModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_listing(std::make_shared<const core::CollectionListing>())
{}

void CollectionWallModel::setListing(std::shared_ptr<const core::CollectionListing> listing)
{
    beginResetModel();
    m_listing = listing ? std::move(listing) : std::make_shared<const core::CollectionListing>();
    regroup();
    endResetModel();
}

void CollectionWallModel::setOrder(core::CollectionOrder order)
{
    if (order == m_order) {
        return;
    }
    beginResetModel();
    m_order = order;
    regroup();
    endResetModel();
}

void CollectionWallModel::regroup()
{
    m_albumFirst.clear();
    m_albumTracks.clear();
    const QList<CollectionTrack> &tracks = m_listing->tracks;
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const CollectionTrack &track = tracks.at(i);
        if (track.albumId == 0) {
            continue;
        }
        ++m_albumTracks[track.albumId];
        const auto first = m_albumFirst.constFind(track.albumId);
        if (first == m_albumFirst.cend() || core::playsBefore(track, tracks.at(*first))) {
            m_albumFirst.insert(track.albumId, static_cast<int>(i));
        }
    }
    m_sections = core::groupCollection(*m_listing, m_order);
    m_rows.clear();
    for (qsizetype s = 0; s < m_sections.size(); ++s) {
        m_rows.append(Row{.kind = Kind::Header, .section = static_cast<int>(s), .tile = -1});
        const qsizetype tiles = m_sections.at(s).tiles.size();
        for (qsizetype t = 0; t < tiles; ++t) {
            m_rows.append(Row{.kind = Kind::Tile, .section = static_cast<int>(s), .tile = static_cast<int>(t)});
        }
    }
}

const core::CollectionTile *CollectionWallModel::tileAt(Row row) const
{
    if (row.kind != Kind::Tile || row.section < 0 || row.section >= m_sections.size()) {
        return nullptr;
    }
    const QList<core::CollectionTile> &tiles = m_sections.at(row.section).tiles;
    return row.tile >= 0 && row.tile < tiles.size() ? &tiles.at(row.tile) : nullptr;
}

QList<qint64> CollectionWallModel::tracksOf(const QModelIndexList &rows) const
{
    QList<int> sorted;
    for (const QModelIndex &index : rows) {
        if (index.isValid() && index.model() == this && index.row() < m_rows.size()) {
            sorted.append(index.row());
        }
    }
    std::ranges::sort(sorted);
    QList<qint64> tracks;
    QSet<qint64> seen;
    for (const int row : std::as_const(sorted)) {
        const core::CollectionTile *tile = tileAt(m_rows.at(row));
        if (tile == nullptr) {
            continue;
        }
        for (const int index : tile->tracks) {
            const qint64 id = m_listing->tracks.at(index).id;
            if (!seen.contains(id)) {
                seen.insert(id);
                tracks.append(id);
            }
        }
    }
    return tracks;
}

QList<int> CollectionWallModel::rowsWithAny(const QSet<qint64> &tracks) const
{
    QList<int> rows;
    if (tracks.isEmpty()) {
        return rows;
    }
    for (qsizetype row = 0; row < m_rows.size(); ++row) {
        const core::CollectionTile *tile = tileAt(m_rows.at(row));
        if (tile == nullptr) {
            continue;
        }
        const bool any = std::ranges::any_of(
            tile->tracks, [this, &tracks](int index) { return tracks.contains(m_listing->tracks.at(index).id); });
        if (any) {
            rows.append(static_cast<int>(row));
        }
    }
    return rows;
}

int CollectionWallModel::rowOfAlbum(quint64 albumId) const
{
    for (qsizetype row = 0; row < m_rows.size(); ++row) {
        const core::CollectionTile *tile = tileAt(m_rows.at(row));
        if (tile != nullptr && albumId != 0 && tile->albumId == albumId) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

QString CollectionWallModel::qualityText(const CollectionTrack &track)
{
    QString text = format::quality(track.quality);
    if (track.tier == QualityTier::Lossless && !match::isLossless(track.quality.codec)) {
        return tr("%1 · from lossless").arg(text);
    }
    // What the spectrum check found (a fake 320): the tier is lower than the stream says.
    const QualityTier plain = core::qualityTier(track.quality);
    if (track.tier != QualityTier::Unknown && track.tier < plain) {
        QString tier = core::tierName(track.tier);
        if (!tier.isEmpty()) {
            tier[0] = tier.at(0).toLower();
        }
        text = tr("%1 · %2").arg(text, tier);
    }
    return text;
}

int CollectionWallModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

Qt::ItemFlags CollectionWallModel::flags(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return Qt::NoItemFlags;
    }
    // A header reads like any row, but can't be selected: the keys pass over it.
    return m_rows.at(index.row()).kind == Kind::Header
               ? Qt::ItemIsEnabled | Qt::ItemNeverHasChildren
               : Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
}

QVariant CollectionWallModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows.at(index.row());
    if (role == KindRole) {
        return static_cast<int>(row.kind);
    }
    if (row.kind == Kind::Header) {
        return sectionHeaderData(m_sections.at(row.section), role);
    }
    const core::CollectionTile *tile = tileAt(row);
    return tile != nullptr ? tileData(*tile, role) : QVariant();
}

QVariant CollectionWallModel::sectionHeaderData(const core::CollectionSection &section, int role) const
{
    switch (role) {
    case Qt::DisplayRole:
        return section.title;
    case SubtitleRole:
        return countOfTracks(section.trackCount);
    case Qt::AccessibleTextRole:
        return tr("%1, %2").arg(section.title, countOfTracks(section.trackCount));
    default:
        break;
    }
    return {};
}

QString CollectionWallModel::coverKey(const CollectionTrack &track)
{
    return u"track:%1:%2:%3"_s.arg(track.id).arg(track.sizeBytes).arg(track.modifiedMs);
}

QString CollectionWallModel::albumCoverKey(quint64 albumId, const CollectionTrack &first)
{
    return u"album:%1:%2"_s.arg(albumId).arg(coverKey(first));
}

const CollectionTrack &CollectionWallModel::albumFirst(quint64 albumId) const
{
    return m_listing->tracks.at(m_albumFirst.value(albumId));
}

QString CollectionWallModel::albumDetail(const core::CollectionTile &tile, const core::CollectionAlbum &album) const
{
    QStringList parts{tr("Album")};
    if (album.year > 0) {
        parts.append(QString::number(album.year));
    }
    const int all = m_albumTracks.value(album.id);
    parts.append(tile.tracks.size() == all ? countOfTracks(all)
                                           : tr("%1 of %2").arg(tile.tracks.size()).arg(countOfTracks(all)));
    return parts.join(u" · "_s);
}

QString CollectionWallModel::albumToolTip(const core::CollectionTile &tile, const core::CollectionAlbum &album) const
{
    return QStringList{album.artist.isEmpty() ? albumTitle(album) : tr("%1 — %2").arg(album.artist, albumTitle(album)),
                       albumDetail(tile, album), QDir::toNativeSeparators(album.folder)}
        .join(u'\n');
}

core::QualityTier CollectionWallModel::lowestTier(const core::CollectionTile &tile) const
{
    QualityTier lowest = m_listing->tracks.at(tile.tracks.constFirst()).tier;
    for (const int index : tile.tracks) {
        lowest = std::min(lowest, m_listing->tracks.at(index).tier);
    }
    return lowest;
}

QList<qint64> CollectionWallModel::trackIds(const core::CollectionTile &tile) const
{
    QList<qint64> ids;
    ids.reserve(tile.tracks.size());
    for (const int index : tile.tracks) {
        ids.append(m_listing->tracks.at(index).id);
    }
    return ids;
}

QVariant CollectionWallModel::tileData(const core::CollectionTile &tile, int role) const
{
    if (tile.tracks.isEmpty()) {
        return {};
    }
    const CollectionTrack &first = m_listing->tracks.at(tile.tracks.constFirst());
    const core::CollectionAlbum *album = tile.albumId != 0 ? m_listing->album(tile.albumId) : nullptr;
    switch (role) {
    case Qt::DisplayRole:
        return album ? albumTitle(*album) : titleOf(first);
    case SubtitleRole:
        return album ? album->artist : first.artist;
    case DetailRole:
        return album ? albumDetail(tile, *album) : qualityText(first);
    case TierRole:
        return static_cast<int>(lowestTier(tile));
    case TrackIdsRole:
        return QVariant::fromValue(trackIds(tile));
    case AlbumIdRole:
        return QVariant::fromValue(tile.albumId);
    case CoverKeyRole:
        return album ? albumCoverKey(tile.albumId, albumFirst(tile.albumId)) : coverKey(first);
    case CoverFolderRole:
        return album ? album->folder : QString();
    case CoverFileRole:
        return album ? albumFirst(tile.albumId).path : first.path;
    case Qt::ToolTipRole: // names from files, as they are
        return format::plainToolTip(album ? albumToolTip(tile, *album) : trackToolTip(first));
    case Qt::AccessibleTextRole:
        if (album) {
            return tr("%1, album by %2, %3").arg(albumTitle(*album), album->artist, albumDetail(tile, *album));
        }
        return first.artist.isEmpty() ? tr("%1, %2").arg(titleOf(first), qualityText(first))
                                      : tr("%1, by %2, %3").arg(titleOf(first), first.artist, qualityText(first));
    default:
        break;
    }
    return {};
}

} // namespace selecta::app
