// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStyledItemDelegate>

#include <array>

class QPalette;

// How Selecta shows a status (P-003, mock/README.md "Statuses"): an SF Symbol, monochrome, tinted with the colour of
// its kind, then a short word in the label colour. The colour says the kind; the symbol and the word say which one.
namespace selecta::app::status {

enum class Kind : quint8 {
    Waiting, // grey: waiting, queued, no release
    Working, // blue: looking up, searching, downloading, converting
    Release, // indigo: release found
    Ready,   // green: ready, saved
    Check,   // orange: check, below your quality, release unsure
    Failed,  // red: not found, download failed
    Have,    // purple: you have it
    Inbox,   // cyan: in your Inbox
    Upgrade, // teal: a better copy of a file you have
};

// A status's symbol: an SF Symbol name (kSymbols lists the ones Selecta uses) and the kind that colours it. The words
// beside it are the model's display text.
struct Status
{
    QString symbol;
    Kind kind = Kind::Waiting;
};

// The SF Symbols statuses use. Each has a drawn stand-in for platforms without SF Symbols.
inline constexpr std::array<const char *, 17> kSymbols = {
    "clock",
    "hourglass",
    "magnifyingglass",
    "opticaldisc.fill",
    "checkmark.circle.fill",
    "checkmark.seal.fill",
    "tray.full.fill",
    "exclamationmark.triangle.fill",
    "xmark.circle.fill",
    "exclamationmark.octagon.fill",
    "arrow.down.circle.fill",
    "arrow.up.circle.fill",
    "arrow.triangle.2.circlepath",
    "questionmark.circle.fill",
    "minus.circle.fill",
    "trash",
    "flag.fill",
};

// The kind's colour in light or dark: the macOS system colour (NSColor.systemGreenColor and the rest, resolved in Aqua
// or Dark Aqua), else the same values as constants. Main thread only (AppKit).
QColor color(Kind kind, bool dark);
// The same for this palette's appearance.
QColor color(Kind kind, const QPalette &palette);

#ifdef Q_OS_MACOS
// The system colour for a Kind (given as int), resolved in Aqua or Dark Aqua, in sRGB; invalid if AppKit can't give it.
// AppearanceMac.mm.
QColor macStatusColor(int kind, bool dark);
#endif

// The symbol: the system's SF Symbol on macOS, else Selecta's drawing of it (the names in kSymbols); a null icon for
// any other name off macOS. Main thread only.
QIcon icon(const QString &symbol);
// The symbol at `size` (device-independent pixels) in `color`. A filled symbol's glyph stays cut out, so the row's
// background, or its selection, shows through it. Null for an unknown symbol.
QPixmap pixmap(const QString &symbol, const QColor &color, int size, qreal devicePixelRatio);
// Selecta's drawing alone, whatever the platform (tests compare it with the system's).
QPixmap drawnPixmap(const QString &symbol, const QColor &color, int size, qreal devicePixelRatio);

// Item roles a model answers for a status column: the symbol's name (QString) and its Kind (int). High numbers, clear
// of the models' own roles.
inline constexpr int kSymbolRole = Qt::UserRole + 0x700;
inline constexpr int kKindRole = Qt::UserRole + 0x701;

// Paints a status cell: the symbol tinted with its kind's colour, then the display text. In a selected row the symbol
// is drawn in the selection's text colour. With symbolOnly, the words go to the tooltip (the model's ToolTipRole, else
// the display text) and the cell shows the symbol alone, centred. Cells with no symbol paint as plain text.
class StatusDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit StatusDelegate(QObject *parent = nullptr, bool symbolOnly = false);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    static constexpr int kSymbolSize = 15;

private:
    bool m_symbolOnly;
};

} // namespace selecta::app::status
