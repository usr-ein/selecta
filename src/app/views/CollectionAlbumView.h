// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/CollectionBrowse.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QStandardItemModel;
class QTreeView;

namespace selecta::app {

class CollectionCovers;
class CoverArt;

// One album of the collection, opened from its tile on the cover wall (P-010): its cover, names and folder, and every
// one of its tracks in disc and track order with its quality, any of which can be selected for "Search for Better
// Quality". Names come from files: shown as plain text. Escape, ⌘[ or the button go back to the wall. Main thread.
class CollectionAlbumView : public QWidget
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        NumberColumn,
        TitleColumn,
        ArtistColumn,
        QualityColumn,
        LengthColumn,
        ColumnCount
    };
    static constexpr int kTrackIdRole = Qt::UserRole + 1; // on every column: the track's CollectionTrack::id
    static constexpr int kPathRole = Qt::UserRole + 2;    // on the title: the track's file
    static constexpr int kCoverSide = 160;

    explicit CollectionAlbumView(CollectionCovers &covers, QWidget *parent = nullptr);

    // Shows this album of `listing`. False, and nothing changes, when the listing has no such album.
    bool showAlbum(const std::shared_ptr<const core::CollectionListing> &listing, quint64 albumId);
    quint64 albumId() const { return m_albumId; }
    QList<qint64> selectedTracks() const;
    QTreeView *trackList() const { return m_tracks; }

Q_SIGNALS:
    void backRequested();
    void selectionChanged();

private:
    void showCover();
    void showMenu(QPoint position);

    CollectionCovers &m_covers;
    quint64 m_albumId = 0;
    QString m_coverKey;
    QString m_folder;
    QString m_firstTrack;
    QPushButton *m_back;
    CoverArt *m_cover;
    QLabel *m_title;
    QLabel *m_artist;
    QLabel *m_details;
    QLabel *m_folderLabel;
    QStandardItemModel *m_model;
    QTreeView *m_tracks;
};

} // namespace selecta::app
