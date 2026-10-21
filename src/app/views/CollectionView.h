// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Albums.h"
#include "core/CollectionService.h"

#include <QHash>
#include <QSet>
#include <QWidget>

#include <memory>

class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;

namespace selecta::core {
class EngineHost;
}
namespace selecta::storage {
class Database;
}

namespace selecta::app {

class CollectionAlbumView;
class CollectionCovers;
class CollectionWall;
class CollectionWallModel;

// The collection (PRD LIB-1, LIB-2, LIB-5, P-010): a wall of covers, one tile per loose track and one per album, grouped
// by month downloaded, artist, genre or quality; above it what the index knows (tracks, albums, size, the tracks folder
// and the albums folder by their roles, formats, the scan in progress) and what needs a look (singles also on an album,
// files that couldn't be read, the Inbox waiting). An album's tile opens its tracks. Tiles and tracks can be selected
// for "Search for Better Quality": each track becomes a Get tracks line looking for a copy of a higher tier, which the
// Inbox marks as an upgrade and filing puts in its place (EngineHost::searchForBetterQuality()).
//
// Selecta never changes these files but on the user's word: an upgrade filed from the Inbox. Covers come from the
// files (CollectionCovers). Main thread. `db` is unused: the listing comes from the engine.
class CollectionView : public QWidget
{
    Q_OBJECT

public:
    CollectionView(core::EngineHost &engine, storage::Database &db, QWidget *parent = nullptr);
    ~CollectionView() override;
    CollectionView(const CollectionView &) = delete;
    CollectionView &operator=(const CollectionView &) = delete;
    CollectionView(CollectionView &&) = delete;
    CollectionView &operator=(CollectionView &&) = delete;

    // One line for the window subtitle.
    QString summary() const;

    CollectionWall *wall() const { return m_wall; }
    CollectionWallModel *wallModel() const { return m_wallModel; }
    CollectionAlbumView *albumView() const { return m_album; }
    // The tracks the selection holds: the album page's, when it's open, else the wall's tiles'.
    QList<qint64> selectedTracks() const;

Q_SIGNALS:
    void summaryChanged();
    void inboxRequested();    // "Open Inbox"
    void settingsRequested(); // "Choose folders…": the folders are chosen in Settings → Library
    // "Search for Better Quality" on these tracks (CollectionTrack::id); the view hands them to the engine.
    void betterQualityRequested(const QList<qint64> &trackIds);

private:
    void showStatus(const core::CollectionStatus &status);
    // DL-8: filed singles that are also on an album.
    void showFlags(const QList<core::SupersedeFlag> &flags);
    void showInbox();
    void showListing(const std::shared_ptr<const core::CollectionListing> &listing);
    void showEmptyText();
    void openAlbum(quint64 albumId);
    void closeAlbum();
    void refreshSelection();
    void searchForBetterQuality(const QList<qint64> &tracks);
    void showSearched(int searching, int already, int best, int gone);
    void showWallMenu(QPoint position);
    // Tracks a better copy can be looked for: those not from lossless.
    QList<qint64> upgradable(const QList<qint64> &tracks) const;

    core::EngineHost &m_engine;
    core::CollectionStatus m_status;
    std::shared_ptr<const core::CollectionListing> m_listing;
    QHash<qint64, core::QualityTier> m_tiers; // by track id, from the listing
    CollectionCovers *m_covers;
    QStackedWidget *m_pages;
    QLabel *m_headline;
    QComboBox *m_order;
    QLabel *m_details;
    QLabel *m_folders;
    QLabel *m_message;
    QProgressBar *m_progress;
    QPushButton *m_scan;
    QLabel *m_unreadableTitle;
    QListWidget *m_unreadable;
    QLabel *m_flagsTitle;
    QListWidget *m_flags;
    QLabel *m_formats;
    QWidget *m_inboxRow;
    QLabel *m_inbox;
    QPushButton *m_openInbox;
    QStackedWidget *m_browse;
    CollectionWallModel *m_wallModel;
    CollectionWall *m_wall;
    CollectionAlbumView *m_album;
    QLabel *m_result;
    QLabel *m_selection;
    QPushButton *m_betterQuality;
};

} // namespace selecta::app
