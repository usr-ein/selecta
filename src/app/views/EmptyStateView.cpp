// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/EmptyStateView.h"

#include "app/Icons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kSymbolSize = 44;
constexpr int kColumnWidth = 460;

QLabel *centeredLabel(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    label->setWordWrap(true);
    return label;
}

} // namespace

EmptyStateView::EmptyStateView(QIcon symbol, const QString &title, const QString &text, const QString &note,
                               QWidget *parent)
    : QWidget(parent)
    , m_symbol(std::move(symbol))
    , m_symbolLabel(new QLabel(this))
{
    m_symbolLabel->setAlignment(Qt::AlignCenter);
    m_symbolLabel->setObjectName(u"empty.symbol"_s);

    // Wrapped labels must span their column: an alignment flag on a layout item would size them to their unwrapped
    // hint and break height-for-width, clipping the text. The column itself is centred with stretches.
    auto *column = new QWidget(this);
    column->setMaximumWidth(kColumnWidth);
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(10);

    auto *titleLabel = centeredLabel(title, u"empty.title"_s, column);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4.0);
    titleFont.setWeight(QFont::DemiBold);
    titleLabel->setFont(titleFont);

    auto *textLabel = centeredLabel(text, u"empty.text"_s, column);
    textLabel->setForegroundRole(QPalette::PlaceholderText);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    columnLayout->addWidget(m_symbolLabel);
    columnLayout->addWidget(titleLabel);
    columnLayout->addWidget(textLabel);
    if (!note.isEmpty()) {
        auto *noteLabel = centeredLabel(note, u"empty.note"_s, column);
        QFont noteFont = noteLabel->font();
        noteFont.setPointSizeF(noteFont.pointSizeF() - 1.0);
        noteLabel->setFont(noteFont);
        noteLabel->setForegroundRole(QPalette::PlaceholderText);
        columnLayout->addSpacing(4);
        columnLayout->addWidget(noteLabel);
    }

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(column, 100);
    row->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->addStretch(2);
    layout->addLayout(row);
    layout->addStretch(3);
    refreshSymbol();
}

void EmptyStateView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) {
        refreshSymbol();
    }
}

void EmptyStateView::refreshSymbol()
{
    const QPixmap pixmap = icons::tinted(m_symbol, QSize(kSymbolSize, kSymbolSize), devicePixelRatioF(),
                                         palette().color(QPalette::PlaceholderText));
    m_symbolLabel->setPixmap(pixmap);
    m_symbolLabel->setVisible(!pixmap.isNull());
}

} // namespace selecta::app
