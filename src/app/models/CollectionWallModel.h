// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/CollectionBrowse.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

namespace selecta::app {

// The cover wall's rows (P-010): each section's header, then its tiles, from the collection's listing grouped one way
// (core::groupCollection()). A flat list, so the wall reads as a list to VoiceOver: a header row says its section
// ("October 2026, 12 tracks"), a tile row its track or album. Names come from files: every text is plain, tooltips
// included (format::plainToolTip()).
//
// Main thread. Changing the listing or the grouping resets the model.
class CollectionWallModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum class Kind : quint8 { Header, Tile };
    enum Role {                      // NOLINT(cppcoreguidelines-use-enum-class): Qt roles are plain ints
        KindRole = Qt::UserRole + 1, // Kind, as an int
        SubtitleRole,                // a tile's artist (an album's artist); a header's track count, in words
        DetailRole,                  // a tile's quality ("MP3 320") or album ("Album · 2024 · 3 of 11 tracks")
        TierRole,                    // a tile's lowest tier (core::QualityTier, as an int)
        TrackIdsRole,                // a tile's tracks (QList<qint64>, CollectionTrack::id), in album order
        AlbumIdRole,                 // a tile's album (quint64), 0 for a loose track
        CoverKeyRole,                // what names a tile's cover: "album:<id>" or "track:<id>"
        CoverFolderRole,             // an album tile's folder, where its cover image may be; empty for a loose track
        CoverFileRole,               // the file whose embedded picture is the tile's cover: its (first) track
    };

    explicit CollectionWallModel(QObject *parent = nullptr);

    void setListing(std::shared_ptr<const core::CollectionListing> listing);
    const std::shared_ptr<const core::CollectionListing> &listing() const { return m_listing; }
    void setOrder(core::CollectionOrder order);
    core::CollectionOrder order() const { return m_order; }
    const QList<core::CollectionSection> &sections() const { return m_sections; }

    // The tracks of these rows (tiles only), each once, in row order.
    QList<qint64> tracksOf(const QModelIndexList &rows) const;
    // The tile rows holding any of these tracks: to select again what was selected before a reset.
    QList<int> rowsWithAny(const QSet<qint64> &tracks) const;
    // The row of this album's first tile, or -1.
    int rowOfAlbum(quint64 albumId) const;

    // The tile's quality in words: "MP3 320", "FLAC 16/44.1"; with what the tier says beyond it ("MP3 320 · from
    // lossless", "MP3 320 · below 192 kbps").
    static QString qualityText(const core::CollectionTrack &track);
    // What names a cover (CollectionCovers): a loose track's, or an album's from its first track as it plays. With the
    // file's size and last change: another file at the same path (an upgrade) is read again.
    static QString coverKey(const core::CollectionTrack &track);
    static QString albumCoverKey(quint64 albumId, const core::CollectionTrack &first);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    struct Row
    {
        Kind kind = Kind::Header;
        int section = 0;
        int tile = -1; // -1 for a header
    };

    void regroup();
    const core::CollectionTile *tileAt(const Row &row) const;
    QVariant tileData(const core::CollectionTile &tile, int role) const;
    QVariant sectionHeaderData(const core::CollectionSection &section, int role) const;
    // An album's first track as it plays; its tile's text, tooltip and lowest tier.
    const core::CollectionTrack &albumFirst(quint64 albumId) const;
    QString albumDetail(const core::CollectionTile &tile, const core::CollectionAlbum &album) const;
    QString albumToolTip(const core::CollectionTile &tile, const core::CollectionAlbum &album) const;
    core::QualityTier lowestTier(const core::CollectionTile &tile) const;
    QList<qint64> trackIds(const core::CollectionTile &tile) const;

    std::shared_ptr<const core::CollectionListing> m_listing;
    core::CollectionOrder m_order = core::CollectionOrder::Added;
    QList<core::CollectionSection> m_sections;
    QList<Row> m_rows;
    QHash<quint64, int> m_albumFirst;  // album id → its first track as it plays (an index into the listing's tracks)
    QHash<quint64, int> m_albumTracks; // album id → how many tracks it has
};

} // namespace selecta::app
