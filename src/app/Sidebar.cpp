// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Sidebar.h"

#include "app/Accessibility.h"
#include "app/Icons.h"
#include "app/NavDelegate.h"
#include "app/NavModel.h"
#include "app/SourceListView.h"

#include <QAction>
#include <QCompleter>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QShortcut>
#include <QStringListModel>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kDotSize = 8;
const QColor kOnlineGreen(0x34, 0xC7, 0x59);

} // namespace

StatusDot::StatusDot(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kDotSize, kDotSize);
    setAccessibleName(tr("Offline"));
}

void StatusDot::setOnline(bool online)
{
    if (m_online == online) {
        return;
    }
    m_online = online;
    setAccessibleName(online ? tr("Online") : tr("Offline"));
    update();
}

void StatusDot::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_online ? kOnlineGreen : palette().color(QPalette::PlaceholderText));
    painter.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}

Sidebar::Sidebar(NavModel *model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_search(new QLineEdit(this))
    , m_recent(new QStringListModel(this))
    , m_list(new SourceListView(this))
    , m_statusDot(new StatusDot(this))
    , m_statusText(new QLabel(this))
{
    setObjectName(u"sidebar"_s);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setMinimumWidth(180);

    // Searches typed by hand start here (SRCH-1); each stays open in the list below.
    m_search->setObjectName(u"sidebar.search"_s);
    m_search->setPlaceholderText(tr("Search Soulseek"));
    m_search->setAccessibleName(tr("Search Soulseek"));
    a11y::addClearButton(m_search);
    m_search->setAttribute(Qt::WA_MacShowFocusRect, true);
    m_search->addAction(icons::symbol(u"magnifyingglass"_s, u"edit-find"_s), QLineEdit::LeadingPosition)
        ->setText(tr("Search"));
    m_search->setToolTip(tr("Search the Soulseek network (⌘F). Use -word to leave a word out."));
    auto *completer = new QCompleter(m_recent, m_search);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    m_search->setCompleter(completer);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_search->text().simplified();
        if (!text.isEmpty()) {
            Q_EMIT searchRequested(text);
            m_search->clear();
        }
    });

    m_list->setObjectName(u"sidebar.list"_s);
    m_list->setModel(m_model);
    auto *delegate = new NavDelegate(m_list);
    m_list->setItemDelegate(delegate);
    m_list->setAccessibleName(tr("Sidebar"));
    m_list->setMouseTracking(true); // hover shows the close buttons
    connect(delegate, &NavDelegate::closeRequested, this, &Sidebar::closeRequested);
    // ⌫ closes the selected search or browse, as the × does under the mouse.
    for (const QKeySequence &keys : {QKeySequence(Qt::Key_Backspace), QKeySequence(Qt::Key_Delete)}) {
        auto *close = new QShortcut(keys, m_list);
        close->setContext(Qt::WidgetShortcut);
        connect(close, &QShortcut::activated, this, [this] {
            const QModelIndex current = m_list->currentIndex();
            if (current.data(NavModel::ClosableRole).toBool()) {
                Q_EMIT closeRequested(current.data(NavModel::IdRole).toString());
            }
        });
    }
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](QPoint position) {
        const QModelIndex index = m_list->indexAt(position);
        if (!index.data(NavModel::ClosableRole).toBool()) {
            return;
        }
        const QString id = index.data(NavModel::IdRole).toString();
        QMenu menu(this);
        menu.addAction(NavModel::searchOfItem(id) > 0 ? tr("Close Search") : tr("Close"), this,
                       [this, id] { Q_EMIT closeRequested(id); });
        menu.exec(m_list->viewport()->mapToGlobal(position));
    });
    a11y::addMenuKey(m_list);

    m_statusDot->setObjectName(u"sidebar.statusDot"_s);
    m_statusText->setObjectName(u"sidebar.status"_s);
    m_statusText->setTextFormat(Qt::PlainText); // a username, the server's words
    m_statusText->setForegroundRole(QPalette::PlaceholderText);

    auto *status = new QHBoxLayout;
    status->setContentsMargins(16, 8, 16, 10);
    status->setSpacing(6);
    status->addWidget(m_statusDot);
    status->addWidget(m_statusText, 1);

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(10, 0, 10, 6);
    searchRow->addWidget(m_search);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(searchRow);
    layout->addWidget(m_list, 1);
    layout->addLayout(status);

    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
        if (m_selecting || !current.isValid()) {
            return;
        }
        const QString id = m_model->idAt(current.row());
        if (!id.isEmpty()) {
            Q_EMIT navigationRequested(id);
        }
    });

    setConnectionStatus(tr("Not connected"), false);
}

void Sidebar::select(const QString &id)
{
    const int row = m_model->rowOf(id);
    m_selecting = true;
    if (row < 0) {
        m_list->selectionModel()->clear();
    } else {
        m_list->selectionModel()->setCurrentIndex(m_model->index(row), QItemSelectionModel::ClearAndSelect);
    }
    m_selecting = false;
}

QString Sidebar::currentId() const
{
    const QModelIndex current = m_list->currentIndex();
    return current.isValid() ? m_model->idAt(current.row()) : QString();
}

void Sidebar::setRecentSearches(const QStringList &searches)
{
    m_recent->setStringList(searches);
}

void Sidebar::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void Sidebar::setConnectionStatus(const QString &text, bool online)
{
    m_statusText->setText(text);
    m_statusDot->setOnline(online);
}

} // namespace selecta::app
