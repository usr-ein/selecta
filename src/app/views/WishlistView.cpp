// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/WishlistView.h"

#include "app/Format.h"
#include "core/EngineHost.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kIdRole = Qt::UserRole + 1;
enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
    SearchColumn,
    LastRunColumn,
    FoundColumn,
    ColumnCount
};

} // namespace

WishlistView::WishlistView(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_field(new QLineEdit(this))
    , m_add(new QPushButton(tr("Add"), this))
    , m_pages(new QStackedWidget(this))
    , m_model(new QStandardItemModel(0, ColumnCount, this))
    , m_table(new QTreeView(this))
{
    setObjectName(u"view.wishlist"_s);
    m_field->setObjectName(u"wishlist.field"_s);
    m_field->setPlaceholderText(tr("Artist and title, or any words to watch for"));
    m_field->setClearButtonEnabled(true);
    m_add->setObjectName(u"wishlist.add"_s);
    connect(m_field, &QLineEdit::returnPressed, this, &WishlistView::addTyped);
    connect(m_add, &QPushButton::clicked, this, &WishlistView::addTyped);
    auto *note = new QLabel(tr("Each search here runs again in the background, one at a time at the pace the server "
                               "sets, and says when it finds files it hadn't seen. Searches you type always go "
                               "first."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    m_model->setHorizontalHeaderLabels({tr("Search"), tr("Last run"), tr("New files")});
    m_table->setObjectName(u"wishlist.items"_s);
    m_table->setModel(m_model);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(SearchColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(LastRunColumn, 140);
    m_table->setColumnWidth(FoundColumn, 120);
    m_table->installEventFilter(this);
    connect(m_table, &QTreeView::doubleClicked, this, &WishlistView::showSelected);
    connect(m_table, &QTreeView::customContextMenuRequested, this, [this](QPoint position) {
        const QList<qint64> ids = selectedIds();
        if (ids.isEmpty()) {
            return;
        }
        QMenu menu(this);
        menu.addAction(tr("Show"), this, &WishlistView::showSelected);
        menu.addSeparator();
        menu.addAction(ids.size() == 1 ? tr("Remove from Wishlist") : tr("Remove %1 from Wishlist").arg(ids.size()),
                       this, [this, ids] {
                           for (const qint64 id : ids) {
                               m_engine.removeFromWishlist(id);
                           }
                       });
        menu.exec(m_table->viewport()->mapToGlobal(position));
    });

    auto *empty = new QLabel(tr("No saved searches. Type one above, or use Add to Wishlist in a search or on a Get "
                                "tracks line nothing was found for."),
                             this);
    empty->setObjectName(u"wishlist.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);
    m_pages->addWidget(m_table);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(16, 10, 16, 4);
    bar->addWidget(m_field, 1);
    bar->addWidget(m_add);
    auto *noteBox = new QHBoxLayout;
    noteBox->setContentsMargins(16, 0, 16, 8);
    noteBox->addWidget(note);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addLayout(noteBox);
    layout->addWidget(m_pages, 1);

    connect(&m_engine, &core::EngineHost::wishlistChanged, this, &WishlistView::rebuild);
    rebuild(m_engine.wishlist());
}

void WishlistView::addTyped()
{
    const QString text = m_field->text().simplified();
    if (text.isEmpty()) {
        return;
    }
    m_engine.addToWishlist(text);
    m_field->clear();
}

void WishlistView::rebuild(const QList<core::WishlistItem> &items)
{
    const QList<qint64> selected = selectedIds();
    m_items = items;
    m_model->removeRows(0, m_model->rowCount());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const core::WishlistItem &item : items) {
        auto *search = new QStandardItem(item.text);
        search->setData(item.id, kIdRole);
        auto *lastRun = new QStandardItem(item.lastRunMs > 0 ? format::ago(item.lastRunMs, now) : tr("Not yet"));
        lastRun->setForeground(QPalette().brush(QPalette::PlaceholderText));
        QString news;
        if (item.found > 0) {
            news = item.found == 1 ? tr("1 new") : tr("%1 new").arg(item.found);
        }
        auto *found = new QStandardItem(news);
        if (item.found > 0) {
            QFont bold = found->font();
            bold.setBold(true);
            found->setFont(bold);
            found->setToolTip(tr("Double-click to see them"));
        }
        m_model->appendRow({search, lastRun, found});
        if (selected.contains(item.id)) {
            m_table->selectionModel()->select(search->index(), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    m_pages->setCurrentIndex(items.isEmpty() ? 0 : 1);
    Q_EMIT summaryChanged();
}

QList<qint64> WishlistView::selectedIds() const
{
    QList<qint64> ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows(SearchColumn);
    for (const QModelIndex &row : rows) {
        ids.append(row.data(kIdRole).toLongLong());
    }
    return ids;
}

void WishlistView::showSelected()
{
    const QModelIndex current = m_table->currentIndex().siblingAtColumn(SearchColumn);
    if (!current.isValid()) {
        return;
    }
    m_engine.markWishlistSeen(current.data(kIdRole).toLongLong());
    Q_EMIT searchRequested(current.data().toString());
}

int WishlistView::foundCount() const
{
    int found = 0;
    for (const core::WishlistItem &item : m_items) {
        found += item.found;
    }
    return found;
}

QString WishlistView::summary() const
{
    if (m_items.isEmpty()) {
        return tr("No saved searches");
    }
    QString searches = m_items.size() == 1 ? tr("1 saved search") : tr("%1 saved searches").arg(m_items.size());
    const int found = foundCount();
    if (found == 0) {
        return searches;
    }
    return found == 1 ? tr("%1 · 1 new file").arg(searches) : tr("%1 · %2 new files").arg(searches).arg(found);
}

bool WishlistView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table && event->type() == QEvent::KeyPress) {
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        if (key && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            showSelected();
            return true;
        }
        if (key && (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete)) {
            const QList<qint64> ids = selectedIds();
            for (const qint64 id : ids) {
                m_engine.removeFromWishlist(id);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace selecta::app
