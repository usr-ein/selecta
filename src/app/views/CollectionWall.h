// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QListView>
#include <QRect>
#include <QSize>
#include <QStyledItemDelegate>

namespace selecta::app {

class CollectionCovers;

// Draws the cover wall's rows (CollectionWallModel): a header's title and track count; a tile's cover (an album's with
// a stack of plates above it), title, artist, and quality with a dot of its tier's colour (the words say it too, PRD
// UI-4). Covers come from CollectionCovers, asked for as tiles are drawn; until one is read, a placeholder. Main thread.
class CollectionTileDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    static constexpr int kStack = 6;    // the album's plates above its cover
    static constexpr int kTextGap = 6;  // between the cover and the title
    static constexpr int kRadius = 6;   // the cover's corners

    CollectionTileDelegate(CollectionCovers &covers, QObject *parent = nullptr);

    // A tile `width` wide, in `font`: the cover, square, and three lines of text below it.
    static int tileHeight(int width, const QFont &font);
    static int headerHeight(const QFont &font);
    // The colour of a tier's dot (core::QualityTier, as an int).
    static QColor tierColour(int tier);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    void paintHeader(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    void paintTile(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;

    CollectionCovers &m_covers;
};

// The cover wall (P-010): CollectionWallModel's headers across the width, each section's tiles in a grid below its
// header, as many to a row as fit at least kMinTileWidth wide, widened to fill the row up to kMaxTileWidth. A QListView,
// so VoiceOver reads it as a list, with a layout of its own.
//
// Keys: the arrows move between tiles (headers are passed over), Home and End go to the first and last, Page Up and
// Page Down a screen away; Return or a double-click on an album opens it (albumOpened()); ⌘A selects every tile. A drag
// from the background selects the tiles it covers.
//
// Main thread. Tile geometry follows the model's resets and the width; covers repaint as they arrive.
class CollectionWall : public QListView
{
    Q_OBJECT

public:
    static constexpr int kMinTileWidth = 150;
    static constexpr int kMaxTileWidth = 220;
    static constexpr int kSpacing = 18;        // between the tiles of a row
    static constexpr int kRowSpacing = 20;     // between rows of tiles
    static constexpr int kMargin = 24;         // left and right, as the page's own
    static constexpr int kSectionSpacing = 12; // above a header that follows tiles
    static constexpr int kHeaderGap = 8;       // between a header and its tiles

    explicit CollectionWall(CollectionCovers &covers, QWidget *parent = nullptr);

    int columns() const { return m_columns; }
    QSize tileSize() const { return m_tile; }
    // Shown in the middle when there's no row: "No tracks in these folders yet."
    void setEmptyText(const QString &text);

    QRect visualRect(const QModelIndex &index) const override;
    void scrollTo(const QModelIndex &index, ScrollHint hint = EnsureVisible) override;
    QModelIndex indexAt(const QPoint &point) const override;
    void setModel(QAbstractItemModel *model) override;
    void reset() override;
    void doItemsLayout() override;

Q_SIGNALS:
    // Return or a double-click on an album's tile.
    void albumOpened(quint64 albumId);

protected:
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
    int horizontalOffset() const override;
    int verticalOffset() const override;
    bool isIndexHidden(const QModelIndex &index) const override;
    void setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command) override;
    QRegion visualRegionForSelection(const QItemSelection &selection) const override;
    void updateGeometries() override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void rowsInserted(const QModelIndex &parent, int start, int end) override;
    void rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end) override;

private:
    // Places every row (content coordinates) for the viewport's width, unless it is placed for it already.
    void ensureLayout() const;
    void layoutRows() const;
    // The first row whose rectangle reaches down to `y` (content coordinates).
    int firstRowReaching(int y) const;
    bool isHeaderRow(int row) const;
    QModelIndex tileIndex(int row) const;
    int firstTile() const;
    int lastTile() const;
    // The tile in the next row of tiles above (direction -1) or below (+1) nearest to `row`'s column; -1 if none.
    int tileInNextRow(int row, int direction) const;
    // The tile nearest to the point (content coordinates), headers aside; -1 if none.
    int tileNear(QPoint point) const;
    void openAlbum(const QModelIndex &index);

    // Layout, placed lazily (mutable: visualRect() and indexAt() are const and may be first to need it).
    mutable QList<QRect> m_rects;
    mutable QList<bool> m_headers;
    mutable int m_contentHeight = 0;
    mutable int m_columns = 1;
    mutable QSize m_tile;
    mutable int m_layoutWidth = -1;
    mutable bool m_dirty = true;
    // The band a drag from the background draws (content coordinates).
    bool m_banding = false;
    QPoint m_bandOrigin;
    QRect m_band;
    QString m_emptyText;
};

} // namespace selecta::app
