// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/CollectionWall.h"

#include "app/Accessibility.h"
#include "app/Appearance.h"
#include "app/models/CollectionWallModel.h"
#include "app/views/CollectionCovers.h"
#include "core/CollectionBrowse.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyleOption>

#include <algorithm>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using Kind = CollectionWallModel::Kind;
using core::QualityTier;

constexpr int kTopMargin = 12;
constexpr int kBottomMargin = 24;

QFont titleFont(const QFont &base)
{
    QFont font = base;
    font.setWeight(QFont::Medium);
    return font;
}

QFont smallFont(const QFont &base)
{
    QFont font = base;
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(std::max(8.0, font.pointSizeF() - 1.0));
    }
    return font;
}

QFont headerFont(const QFont &base)
{
    QFont font = base;
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() + 2.0);
    }
    font.setWeight(QFont::Bold);
    return font;
}

// `amount` of the way from the ground's colour to the ink's: placeholder fills that suit light and dark.
QColor blend(const QPalette &palette, qreal amount)
{
    const QColor ground = palette.color(QPalette::Base);
    const QColor ink = palette.color(QPalette::Text);
    return QColor::fromRgbF(static_cast<float>(ground.redF() + ((ink.redF() - ground.redF()) * amount)),
                            static_cast<float>(ground.greenF() + ((ink.greenF() - ground.greenF()) * amount)),
                            static_cast<float>(ground.blueF() + ((ink.blueF() - ground.blueF()) * amount)));
}

} // namespace

// --- The delegate ----------------------------------------------------------------------------------------------------

CollectionTileDelegate::CollectionTileDelegate(CollectionCovers &covers, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_covers(covers)
{}

int CollectionTileDelegate::tileHeight(int width, const QFont &font)
{
    return kStack + width + kTextGap + QFontMetrics(titleFont(font)).height() +
           (2 * QFontMetrics(smallFont(font)).height()) + 2;
}

int CollectionTileDelegate::headerHeight(const QFont &font)
{
    return QFontMetrics(headerFont(font)).height() + 4;
}

QSize CollectionTileDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (index.data(CollectionWallModel::KindRole).toInt() == static_cast<int>(Kind::Header)) {
        return {CollectionWall::kMinTileWidth, headerHeight(option.font)};
    }
    return {CollectionWall::kMinTileWidth, tileHeight(CollectionWall::kMinTileWidth, option.font)};
}

void CollectionTileDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    if (index.data(CollectionWallModel::KindRole).toInt() == static_cast<int>(Kind::Header)) {
        paintHeader(painter, option, index);
    } else {
        paintTile(painter, option, index);
    }
    painter->restore();
}

void CollectionTileDelegate::paintHeader(QPainter *painter, const QStyleOptionViewItem &option,
                                         const QModelIndex &index) const
{
    const QFont title = headerFont(option.font);
    const QFontMetrics titleMetrics(title);
    const QString text =
        titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, option.rect.width() * 2 / 3);
    painter->setFont(title);
    painter->setPen(option.palette.color(QPalette::Text));
    const int baseline = option.rect.bottom() - titleMetrics.descent() - 2;
    painter->drawText(QPoint(option.rect.left(), baseline), text);
    // The track count after it, smaller, in the secondary colour.
    const QFont count = smallFont(option.font);
    painter->setFont(count);
    painter->setPen(option.palette.color(QPalette::PlaceholderText));
    painter->drawText(QPoint(option.rect.left() + titleMetrics.horizontalAdvance(text) + 10, baseline),
                      index.data(CollectionWallModel::SubtitleRole).toString());
}

void CollectionTileDelegate::paintTile(QPainter *painter, const QStyleOptionViewItem &option,
                                       const QModelIndex &index) const
{
    const QRect rect = option.rect;
    const int side = rect.width();
    const QRectF cover(rect.left(), rect.top() + kStack, side, side);
    const bool album = index.data(CollectionWallModel::AlbumIdRole).toULongLong() != 0;
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const QPalette &palette = option.palette;

    // Selected: a soft backdrop behind the whole tile, in the accent colour (grey while the window isn't active, as
    // macOS does), and a ring around the cover below.
    if (selected) {
        QColor backdrop = palette.color(QPalette::Highlight);
        backdrop.setAlphaF(0.22F);
        painter->setPen(Qt::NoPen);
        painter->setBrush(backdrop);
        painter->drawRoundedRect(QRectF(rect).adjusted(-7, -5, 7, 5), kRadius + 4, kRadius + 4);
    }

    // An album: two plates behind its cover, like a stack of sleeves.
    if (album) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(blend(palette, 0.22));
        painter->drawRoundedRect(cover.adjusted(12, -kStack, -12, -side + 2), 2, 2);
        painter->setBrush(blend(palette, 0.32));
        painter->drawRoundedRect(cover.adjusted(6, -(kStack / 2.0), -6, -side + 2), 2, 2);
    }

    QPainterPath shape;
    shape.addRoundedRect(cover, kRadius, kRadius);
    const QString key = index.data(CollectionWallModel::CoverKeyRole).toString();
    const std::optional<QPixmap> pixmap = m_covers.cover(key);
    if (!pixmap) {
        m_covers.request(key, CollectionCovers::Source{index.data(CollectionWallModel::CoverFolderRole).toString(),
                                                       index.data(CollectionWallModel::CoverFileRole).toString()});
    }
    if (pixmap && !pixmap->isNull()) {
        painter->setClipPath(shape);
        painter->drawPixmap(cover, *pixmap, QRectF(pixmap->rect()));
        painter->setClipping(false);
    } else {
        painter->fillPath(shape, blend(palette, 0.07));
        QFont glyph = option.font;
        glyph.setPixelSize(std::max(1, side * 2 / 7));
        painter->setFont(glyph);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(cover, Qt::AlignCenter, album ? u"♫"_s : u"♪"_s); // ♫ ♪
    }
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(blend(palette, 0.14), 1.0));
    painter->drawPath(shape);

    // Selected: a ring of the accent colour around the cover; the current tile, a thinner one.
    if (selected || (option.state & QStyle::State_HasFocus) != 0) {
        const QColor accent = palette.color(QPalette::Highlight);
        painter->setPen(QPen(accent, selected ? 3.0 : 1.5));
        const qreal out = selected ? 2.5 : 1.5;
        painter->drawRoundedRect(cover.adjusted(-out, -out, out, out), kRadius + out, kRadius + out);
    }

    // Title, artist, quality.
    const int width = rect.width();
    int y = static_cast<int>(cover.bottom()) + kTextGap;
    const QFont title = titleFont(option.font);
    const QFontMetrics titleMetrics(title);
    painter->setFont(title);
    painter->setPen(palette.color(QPalette::Text));
    painter->drawText(QRect(rect.left(), y, width, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, width));
    y += titleMetrics.height();
    const QFont small = smallFont(option.font);
    const QFontMetrics smallMetrics(small);
    painter->setFont(small);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(
        QRect(rect.left(), y, width, smallMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
        smallMetrics.elidedText(index.data(CollectionWallModel::SubtitleRole).toString(), Qt::ElideRight, width));
    y += smallMetrics.height();
    // The tier's dot, then its words: the colour never says it alone.
    const int dot = std::max(6, (smallMetrics.height() / 2) - 1);
    painter->setPen(Qt::NoPen);
    painter->setBrush(qualityTierColor(index.data(CollectionWallModel::TierRole).toInt(), palette));
    painter->drawEllipse(QRectF(rect.left() + 1, y + ((smallMetrics.height() - dot) / 2.0), dot, dot));
    painter->setPen(palette.color(QPalette::PlaceholderText));
    const int textLeft = rect.left() + dot + 6;
    painter->drawText(QRect(textLeft, y, width - dot - 6, smallMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      smallMetrics.elidedText(index.data(CollectionWallModel::DetailRole).toString(), Qt::ElideRight,
                                              width - dot - 6));
}

// --- The wall --------------------------------------------------------------------------------------------------------

CollectionWall::CollectionWall(CollectionCovers &covers, QWidget *parent)
    : QListView(parent)
{
    setItemDelegate(new CollectionTileDelegate(covers, this));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionRectVisible(false); // the band is drawn here: QListView's own needs its own layout
    setMovement(QListView::Static);
    setDragEnabled(false);
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setFrameShape(QFrame::NoFrame);
    setUniformItemSizes(false);
    setMouseTracking(false);
    setContextMenuPolicy(Qt::CustomContextMenu);
    viewport()->setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Base);
    connect(&covers, &CollectionCovers::ready, viewport(), qOverload<>(&QWidget::update));
    connect(this, &QAbstractItemView::doubleClicked, this, &CollectionWall::openAlbum);
    a11y::onReturn(this, [this] { openAlbum(currentIndex()); });
    a11y::addMenuKey(this);
}

void CollectionWall::setModel(QAbstractItemModel *model)
{
    m_dirty = true;
    QListView::setModel(model);
}

void CollectionWall::reset()
{
    m_dirty = true; // another grouping may have as many rows, with the headers elsewhere
    QListView::reset();
}

void CollectionWall::rowsInserted(const QModelIndex &parent, int start, int end)
{
    m_dirty = true;
    QListView::rowsInserted(parent, start, end);
}

void CollectionWall::rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end)
{
    m_dirty = true;
    QListView::rowsAboutToBeRemoved(parent, start, end);
}

void CollectionWall::setEmptyText(const QString &text)
{
    m_emptyText = text;
    viewport()->update();
}

void CollectionWall::openAlbum(const QModelIndex &index)
{
    const quint64 album = index.isValid() ? index.data(CollectionWallModel::AlbumIdRole).toULongLong() : 0;
    if (album != 0) {
        Q_EMIT albumOpened(album);
    }
}

void CollectionWall::ensureLayout() const
{
    const int rows = model() ? model()->rowCount(rootIndex()) : 0;
    if (m_dirty || m_layoutWidth != viewport()->width() || m_rects.size() != rows) {
        layoutRows();
    }
}

void CollectionWall::layoutRows() const
{
    const int rows = model() ? model()->rowCount(rootIndex()) : 0;
    const int width = viewport()->width();
    const int available = std::max(kMinTileWidth, width - (2 * kMargin));
    m_columns = std::max(1, (available + kSpacing) / (kMinTileWidth + kSpacing));
    const int tileWidth = std::min(kMaxTileWidth, (available - ((m_columns - 1) * kSpacing)) / m_columns);
    m_tile = QSize(tileWidth, CollectionTileDelegate::tileHeight(tileWidth, font()));
    const int headerHeight = CollectionTileDelegate::headerHeight(font());
    m_rects.resize(rows);
    m_headers.resize(rows);
    int y = kTopMargin;
    int column = 0;
    for (int row = 0; row < rows; ++row) {
        const bool header = model()->index(row, 0, rootIndex()).data(CollectionWallModel::KindRole).toInt() ==
                            static_cast<int>(Kind::Header);
        m_headers[row] = header;
        if (header) {
            if (column > 0) { // the last row of the section before
                y += m_tile.height() + kRowSpacing;
                column = 0;
            }
            if (row > 0) {
                y += kSectionSpacing;
            }
            m_rects[row] = QRect(kMargin, y, std::max(1, width - (2 * kMargin)), headerHeight);
            y += headerHeight + kHeaderGap;
            continue;
        }
        m_rects[row] = QRect(kMargin + (column * (tileWidth + kSpacing)), y, tileWidth, m_tile.height());
        if (++column == m_columns) {
            column = 0;
            y += m_tile.height() + kRowSpacing;
        }
    }
    if (column > 0) {
        y += m_tile.height() + kRowSpacing;
    }
    m_contentHeight = rows > 0 ? y + kBottomMargin : 0;
    m_layoutWidth = width;
    m_dirty = false;
}

int CollectionWall::firstRowReaching(int y) const
{
    // Rows go down the page: their bottoms never decrease.
    const QList<QRect> &rects = m_rects;
    const auto it = std::ranges::partition_point(rects, [y](QRect rect) { return rect.bottom() < y; });
    return static_cast<int>(it - rects.cbegin());
}

bool CollectionWall::isHeaderRow(int row) const
{
    return row >= 0 && row < m_headers.size() && m_headers.at(row);
}

QModelIndex CollectionWall::tileIndex(int row) const
{
    return row >= 0 && model() ? model()->index(row, 0, rootIndex()) : QModelIndex();
}

int CollectionWall::firstTile() const
{
    for (int row = 0; row < m_headers.size(); ++row) {
        if (!m_headers.at(row)) {
            return row;
        }
    }
    return -1;
}

int CollectionWall::lastTile() const
{
    for (auto row = static_cast<int>(m_headers.size()) - 1; row >= 0; --row) {
        if (!m_headers.at(row)) {
            return row;
        }
    }
    return -1;
}

int CollectionWall::tileInNextRow(int row, int direction) const
{
    const QRect here = m_rects.at(row);
    const auto count = static_cast<int>(m_rects.size());
    int next = row + direction;
    while (next >= 0 && next < count && (m_headers.at(next) || m_rects.at(next).top() == here.top())) {
        next += direction;
    }
    if (next < 0 || next >= count) {
        return -1;
    }
    const int top = m_rects.at(next).top();
    int best = next;
    for (int candidate = next;
         candidate >= 0 && candidate < count && !m_headers.at(candidate) && m_rects.at(candidate).top() == top;
         candidate += direction) {
        if (std::abs(m_rects.at(candidate).center().x() - here.center().x()) <
            std::abs(m_rects.at(best).center().x() - here.center().x())) {
            best = candidate;
        }
    }
    return best;
}

int CollectionWall::tileNear(QPoint point) const
{
    int best = -1;
    int bestDistance = 0;
    const int first = firstRowReaching(point.y() - m_tile.height() - kRowSpacing);
    for (int row = first; row < m_rects.size() && m_rects.at(row).top() <= point.y() + m_tile.height(); ++row) {
        if (m_headers.at(row)) {
            continue;
        }
        const QPoint delta = m_rects.at(row).center() - point;
        const int distance = (delta.x() * delta.x()) + (delta.y() * delta.y());
        if (best < 0 || distance < bestDistance) {
            best = row;
            bestDistance = distance;
        }
    }
    if (best >= 0) {
        return best;
    }
    return point.y() < 0 ? firstTile() : lastTile();
}

QRect CollectionWall::visualRect(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != model() || index.parent() != rootIndex()) {
        return {};
    }
    ensureLayout();
    if (index.row() >= m_rects.size()) {
        return {};
    }
    return m_rects.at(index.row()).translated(-horizontalOffset(), -verticalOffset());
}

void CollectionWall::scrollTo(const QModelIndex &index, ScrollHint hint)
{
    if (!index.isValid() || index.row() >= (model() ? model()->rowCount(rootIndex()) : 0)) {
        return;
    }
    ensureLayout();
    QRect area = m_rects.at(index.row());
    // The first row of a section shows its header too.
    for (int row = index.row() - 1; row >= 0 && m_rects.at(row).bottom() >= area.top() - kHeaderGap - 1; --row) {
        if (m_headers.at(row)) {
            area = area.united(m_rects.at(row));
            break;
        }
    }
    const int view = viewport()->height();
    const int top = verticalOffset();
    int target = top;
    switch (hint) {
    case PositionAtTop:
        target = area.top() - kTopMargin;
        break;
    case PositionAtBottom:
        target = area.bottom() - view + kBottomMargin;
        break;
    case PositionAtCenter:
        target = area.center().y() - (view / 2);
        break;
    case EnsureVisible:
        if (area.top() < top) {
            target = area.top() - kTopMargin;
        } else if (area.bottom() > top + view) {
            target = area.bottom() - view + kRowSpacing;
        }
        break;
    }
    verticalScrollBar()->setValue(target);
}

QModelIndex CollectionWall::indexAt(const QPoint &point) const
{
    ensureLayout();
    const QPoint content = point + QPoint(horizontalOffset(), verticalOffset());
    for (int row = firstRowReaching(content.y()); row < m_rects.size() && m_rects.at(row).top() <= content.y(); ++row) {
        if (m_rects.at(row).contains(content)) {
            return tileIndex(row);
        }
    }
    return {};
}

QModelIndex CollectionWall::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers /*modifiers*/)
{
    ensureLayout();
    const QModelIndex current = currentIndex();
    if (!current.isValid() || current.row() >= m_rects.size()) {
        return tileIndex(firstTile());
    }
    const int row = current.row();
    if (isHeaderRow(row)) {
        // Somewhere on a header (clicked): the keys start from its first tile.
        for (int next = row + 1; next < m_headers.size(); ++next) {
            if (!m_headers.at(next)) {
                return tileIndex(next);
            }
        }
        return tileIndex(lastTile());
    }
    int target = row;
    switch (cursorAction) {
    case MoveLeft:
    case MovePrevious:
        for (int previous = row - 1; previous >= 0; --previous) {
            if (!m_headers.at(previous)) {
                target = previous;
                break;
            }
        }
        break;
    case MoveRight:
    case MoveNext:
        for (int next = row + 1; next < m_headers.size(); ++next) {
            if (!m_headers.at(next)) {
                target = next;
                break;
            }
        }
        break;
    case MoveUp:
    case MoveDown:
        if (const int next = tileInNextRow(row, cursorAction == MoveUp ? -1 : 1); next >= 0) {
            target = next;
        }
        break;
    case MovePageUp:
    case MovePageDown: {
        const int step = cursorAction == MovePageUp ? -viewport()->height() : viewport()->height();
        if (const int near = tileNear(m_rects.at(row).center() + QPoint(0, step)); near >= 0) {
            target = near;
        }
        break;
    }
    case MoveHome:
        target = firstTile();
        break;
    case MoveEnd:
        target = lastTile();
        break;
    }
    return tileIndex(target);
}

int CollectionWall::horizontalOffset() const
{
    return 0;
}

int CollectionWall::verticalOffset() const
{
    return verticalScrollBar()->value();
}

bool CollectionWall::isIndexHidden(const QModelIndex & /*index*/) const
{
    return false;
}

void CollectionWall::setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command)
{
    if (!selectionModel() || !model()) {
        return;
    }
    ensureLayout();
    const QRect content = rect.normalized().translated(horizontalOffset(), verticalOffset());
    QItemSelection selection;
    int rangeStart = -1;
    int previous = -2;
    const auto flush = [&] {
        if (rangeStart >= 0) {
            selection.select(tileIndex(rangeStart), tileIndex(previous));
        }
    };
    for (int row = firstRowReaching(content.top()); row < m_rects.size() && m_rects.at(row).top() <= content.bottom();
         ++row) {
        if (m_headers.at(row) || !m_rects.at(row).intersects(content)) {
            continue;
        }
        if (row != previous + 1) {
            flush();
            rangeStart = row;
        }
        previous = row;
    }
    flush();
    selectionModel()->select(selection, command);
}

QRegion CollectionWall::visualRegionForSelection(const QItemSelection &selection) const
{
    ensureLayout();
    QRegion region;
    const QRect visible = viewport()->rect();
    const int first = firstRowReaching(verticalOffset());
    const int last = firstRowReaching(verticalOffset() + visible.height() + m_tile.height());
    for (const QItemSelectionRange &range : selection) {
        for (int row = std::max(range.top(), first); row <= std::min(range.bottom(), last); ++row) {
            if (row < m_rects.size()) {
                // With room for the backdrop and the ring drawn around a selected tile.
                region += visualRect(tileIndex(row)).adjusted(-8, -8, 8, 8);
            }
        }
    }
    return region;
}

void CollectionWall::updateGeometries()
{
    ensureLayout();
    const int view = viewport()->height();
    verticalScrollBar()->setSingleStep(std::max(8, m_tile.height() / 6));
    verticalScrollBar()->setPageStep(view);
    verticalScrollBar()->setRange(0, std::max(0, m_contentHeight - view));
    horizontalScrollBar()->setRange(0, 0);
    QAbstractItemView::updateGeometries(); // NOLINT(bugprone-parent-virtual-call): QListView's layout isn't used
}

void CollectionWall::doItemsLayout()
{
    m_dirty = true;
    layoutRows();
    QAbstractItemView::doItemsLayout(); // NOLINT(bugprone-parent-virtual-call): QListView's layout isn't used
}

void CollectionWall::resizeEvent(QResizeEvent *event)
{
    QAbstractItemView::resizeEvent(event); // NOLINT(bugprone-parent-virtual-call): QListView's layout isn't used
    if (viewport()->width() != m_layoutWidth) {
        m_dirty = true;
    }
    updateGeometries();
    viewport()->update();
}

void CollectionWall::scrollContentsBy(int dx, int dy)
{
    viewport()->scroll(dx, dy);
}

void CollectionWall::paintEvent(QPaintEvent *event)
{
    ensureLayout();
    QPainter painter(viewport());
    if (m_rects.isEmpty()) {
        if (!m_emptyText.isEmpty()) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(viewport()->rect().adjusted(kMargin, 0, -kMargin, 0), Qt::AlignCenter | Qt::TextWordWrap,
                             m_emptyText);
        }
        return;
    }
    const QRect exposed = event->rect();
    const int top = verticalOffset() + exposed.top();
    const int bottom = verticalOffset() + exposed.bottom();
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    const QModelIndex current = currentIndex();
    const bool focused = hasFocus();
    const QItemSelectionModel *selected = selectionModel();
    for (int row = firstRowReaching(top); row < m_rects.size() && m_rects.at(row).top() <= bottom; ++row) {
        const QModelIndex index = tileIndex(row);
        option.rect = visualRect(index);
        option.state = QStyle::State_Enabled;
        if (isActiveWindow()) {
            option.state |= QStyle::State_Active;
        }
        if (selected != nullptr && selected->isSelected(index)) {
            option.state |= QStyle::State_Selected;
        }
        if (focused && index == current && !m_headers.at(row)) {
            option.state |= QStyle::State_HasFocus;
        }
        itemDelegateForIndex(index)->paint(&painter, option, index);
    }
    if (m_banding && m_band.isValid()) {
        QStyleOptionRubberBand band;
        band.initFrom(this);
        band.shape = QRubberBand::Rectangle;
        band.opaque = false;
        band.rect = m_band.translated(-horizontalOffset(), -verticalOffset());
        painter.save();
        style()->drawControl(QStyle::CE_RubberBand, &band, &painter);
        painter.restore();
    }
}

void CollectionWall::mousePressEvent(QMouseEvent *event)
{
    const QModelIndex pressed = indexAt(event->position().toPoint());
    m_banding = event->button() == Qt::LeftButton && (!pressed.isValid() || isHeaderRow(pressed.row()));
    m_bandOrigin = event->position().toPoint() + QPoint(horizontalOffset(), verticalOffset());
    m_band = {};
    QListView::mousePressEvent(event);
}

void CollectionWall::mouseMoveEvent(QMouseEvent *event)
{
    QListView::mouseMoveEvent(event);
    if (m_banding && (event->buttons() & Qt::LeftButton) != 0) {
        const QRect before = m_band;
        m_band = QRect(m_bandOrigin, event->position().toPoint() + QPoint(horizontalOffset(), verticalOffset()))
                     .normalized();
        viewport()->update(
            before.united(m_band).translated(-horizontalOffset(), -verticalOffset()).adjusted(-2, -2, 2, 2));
    }
}

void CollectionWall::mouseReleaseEvent(QMouseEvent *event)
{
    QListView::mouseReleaseEvent(event);
    if (m_banding) {
        const QRect before = m_band;
        m_banding = false;
        m_band = {};
        viewport()->update(before.translated(-horizontalOffset(), -verticalOffset()).adjusted(-2, -2, 2, 2));
    }
}

} // namespace selecta::app
