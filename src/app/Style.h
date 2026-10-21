// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMargins>
#include <QPainterPath>
#include <QProxyStyle>
#include <QRectF>

class QAbstractItemView;
class QPainter;
class QStyleOptionHeader;
class QStyleOptionViewItem;
class QPalette;
class QSplitter;

// The look of P-003 (mock/README.md, "The look"): one spacing scale, continuous corners, inset tables.
namespace selecta::app::style {

// The spacing scale, in device-independent pixels: every margin, gap and row height in the views comes from here
// (mock/styles.css --sp-1 … --sp-6).
inline constexpr int kSpace1 = 4;  // an icon and its label, inside a control
inline constexpr int kSpace2 = 8;  // related controls; table and sidebar insets
inline constexpr int kSpace3 = 12; // groups of controls; cell padding
inline constexpr int kSpace4 = 16; // inspector, sheet and group padding
inline constexpr int kSpace5 = 20; // the window margin: content edges, gaps between sections
inline constexpr int kSpace6 = 28; // between page sections

inline constexpr int kRowHeight = 28;        // a one-line table row, a sidebar row
inline constexpr int kTwoLineRowHeight = 44; // a two-line table row
inline constexpr int kBarHeight = 52;        // the toolbar
inline constexpr int kRowInset = kSpace2;    // table rows stand this far in from the view's sides
inline constexpr int kCellPadding = kSpace3; // text in a cell stands this far from the cell's sides

// A view's bar of controls above its table (mock: .viewbar), and a line of text under it (counts, notes: .summary).
// Their sides are the window margin, so the bars' controls and the table's text line up.
inline constexpr QMargins kBarMargins{kSpace5, kSpace2, kSpace5, kSpace3};
inline constexpr QMargins kLineMargins{kSpace5, 0, kSpace5, kSpace2};
// A page of sections (Settings, Shares): the window margin, more room at the bottom.
inline constexpr QMargins kPageMargins{kSpace5, kSpace5, kSpace5, kSpace6};

inline constexpr qreal kRadiusSmall = 6.0;   // row selection, thumbnails
inline constexpr qreal kRadiusMedium = 10.0; // groups, cards, covers
inline constexpr qreal kRadiusLarge = 14.0;  // the floating sidebar, sheets, the waveform screen

// A rounded rectangle with continuous ("squircle") corners, as macOS draws its windows, buttons and icons: the curve
// starts further along each side than a circular arc does and eases into it, so there is no visible joint. A radius of
// half the shorter side or more gives a capsule (round ends, as macOS's capsules have). A radius of zero or less, or
// an empty rectangle, gives a plain rectangle. Pure: any thread.
QPainterPath squircle(const QRectF &rect, qreal radius);

// The same, with only the corners on one side rounded (a row selection that continues into the next cell).
enum class Corners : quint8 { All, Left, Right, None };
QPainterPath squircle(const QRectF &rect, qreal radius, Corners corners);

// The rows of a table: the selection as an inset squircle, no stripes, no grid, no focus ring, and a quiet header with
// one hairline under it. Group boxes as System Settings' groups. Splitters as hairlines. It applies to every QTreeView,
// QListView and QTableView in the app except those that paint their own rows (the sidebar's source list). Installed
// once on the application (main.cpp); widgets built in tests without it keep the platform's look. Main thread only,
// like every style.
class SelectaStyle : public QProxyStyle
{
    Q_OBJECT

public:
    // Wraps the platform's style (QMacStyle on macOS). The application owns the style once installed.
    explicit SelectaStyle(QObject *parent = nullptr);

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption *option,
                         const QWidget *widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                           const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override;
    // Group boxes as System Settings draws its groups: the title above, the controls on a rounded fill.
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter,
                            const QWidget *widget = nullptr) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl,
                         const QWidget *widget = nullptr) const override;
    void polish(QWidget *widget) override;
    using QProxyStyle::polish;

    // Whether the style draws this view's rows (a table, not the source list).
    static bool drawsRowsOf(const QWidget *widget);
    // A view that paints its own rows keeps the platform's: call before it is shown.
    static void keepPlatformRows(QAbstractItemView *view);
    // A splitter whose handles are invisible (the sidebar's: its glass margin is the gap). Other splitters' handles are
    // a hairline.
    static void hideHandles(QSplitter *splitter);

private:
    // A header section's label and sort arrow, in line with the cells' text.
    void drawHeaderLabel(const QStyleOptionHeader &header, QPainter *painter, const QWidget *widget) const;
};

// Fills a cell's share of a selected row: the row is one inset squircle across its cells. `position` is the cell's
// place in the row (QStyleOptionViewItem::ViewItemPosition, Invalid meaning the only cell). Delegates that paint their
// own cells call it, so their rows match the style's.
void paintRowSelection(QPainter *painter, QRect cell, int position, const QColor &fill);
// A cell's rectangle without the inset its row has at the view's sides, for what a delegate draws inside it.
QRect cellContentRect(QRect cell, int position);
// The cell's place in its row as the style paints it (a QStyleOptionViewItem::ViewItemPosition): from the header's
// visual order in a tree whose columns were moved, and after a tree's disclosure triangle when it starts the row.
int cellPosition(const QStyleOptionViewItem &option);

} // namespace selecta::app::style
