// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/RoomsView.h"

#include "app/Accessibility.h"
#include "app/PeerMenu.h"
#include "core/EngineHost.h"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QTextBrowser>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kNameRole = Qt::UserRole + 1;
constexpr int kSortRole = Qt::UserRole + 2;

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

QTreeView *makeList(const QString &name, QAbstractItemModel *model, QWidget *parent)
{
    auto *view = new QTreeView(parent);
    view->setObjectName(name);
    view->setModel(model);
    view->setRootIsDecorated(false);
    view->setUniformRowHeights(true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setFrameShape(QFrame::NoFrame);
    view->setAttribute(Qt::WA_MacShowFocusRect, false);
    view->header()->setStretchLastSection(false);
    view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    return view;
}

QString stateText(const core::Room &room)
{
    switch (room.state) {
    case core::Room::State::Waiting:
        return RoomsView::tr("Waiting to join");
    case core::Room::State::Joining:
        return RoomsView::tr("Joining…");
    case core::Room::State::Failed:
        return room.message;
    case core::Room::State::Joined:
        break;
    }
    return room.members.size() == 1 ? RoomsView::tr("1 member")
                                    : RoomsView::tr("%1 members").arg(QLocale().toString(room.members.size()));
}

} // namespace

RoomsView::RoomsView(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_joinedModel(new QStandardItemModel(0, 2, this))
    , m_joined(makeList(u"rooms.joined"_s, m_joinedModel, this))
    , m_filter(new QLineEdit(this))
    , m_listModel(new QStandardItemModel(0, 2, this))
    , m_listProxy(new QSortFilterProxyModel(this))
    , m_list(makeList(u"rooms.list"_s, m_listProxy, this))
    , m_join(new QPushButton(tr("Join"), this))
    , m_joinProblem(new QLabel(this))
    , m_pages(new QStackedWidget(this))
    , m_name(new QLabel(this))
    , m_details(new QLabel(this))
    , m_leave(new QPushButton(tr("Leave"), this))
    , m_lines(new QTextBrowser(this))
    , m_membersModel(new QStringListModel(this))
    , m_members(new QListView(this))
    , m_problem(new QLabel(this))
    , m_input(new QLineEdit(this))
    , m_send(new QPushButton(tr("Send"), this))
{
    setObjectName(u"view.rooms"_s);
    auto *empty = new QLabel(tr("Join a room from the list to read and post. The server keeps no history: a room "
                                "shows what is said from the moment you join."),
                             this);
    empty->setObjectName(u"rooms.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);
    m_pages->addWidget(createRoomPage());

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(u"rooms.splitter"_s);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(createLeft());
    splitter->addWidget(m_pages);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 600});
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);

    connect(&m_engine, &core::EngineHost::roomListChanged, this, &RoomsView::rebuildList);
    connect(&m_engine, &core::EngineHost::roomChanged, this, [this](const core::Room &room) {
        m_rooms.insert(room.name, room);
        rebuildJoined();
        if (room.name == m_room) {
            refreshRoom();
        }
        Q_EMIT unreadChanged();
        Q_EMIT summaryChanged();
    });
    connect(&m_engine, &core::EngineHost::roomClosed, this, [this](const QString &room) {
        m_rooms.remove(room);
        if (room == m_room) {
            m_room.clear();
            refreshRoom();
        }
        rebuildJoined();
        updateViewing();
        Q_EMIT unreadChanged();
        Q_EMIT summaryChanged();
    });
    connect(&m_engine, &core::EngineHost::roomLineAdded, this, [this](const QString &room, const core::RoomLine &line) {
        if (room == m_room) {
            appendLine(line);
        }
    });
    connect(&m_engine, &core::EngineHost::roomRefused, this, [this](const QString &room, const QString &problem) {
        m_joinProblem->setText(tr("Couldn't join “%1”: %2").arg(room, problem));
        m_joinProblem->show();
    });
    connect(&m_engine, &core::EngineHost::roomSaid, this, [this](const QString &room, const QString &problem) {
        m_send->setEnabled(true);
        if (room != m_room) {
            return;
        }
        if (problem.isEmpty()) {
            m_input->clear();
            m_problem->hide();
        } else {
            m_problem->setText(problem);
            m_problem->show();
        }
    });
    connect(&m_engine, &core::EngineHost::networkStatusChanged, this, &RoomsView::refreshRoom);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, &RoomsView::updateViewing);

    const QList<core::Room> rooms = m_engine.joinedRooms();
    for (const core::Room &room : rooms) {
        m_rooms.insert(room.name, room);
    }
    rebuildJoined();
    rebuildList(m_engine.roomList());
    refreshRoom();
}

QWidget *RoomsView::createLeft()
{
    m_joinedModel->setHorizontalHeaderLabels({tr("Room"), tr("Unread")});
    m_joined->setHeaderHidden(true);
    m_joined->setAccessibleName(tr("Your rooms"));
    connect(m_joined->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) {
        const QString room = index.siblingAtColumn(0).data(kNameRole).toString();
        if (!room.isEmpty() && room != m_room) {
            openRoom(room);
        }
    });

    m_filter->setObjectName(u"rooms.filter"_s);
    m_filter->setPlaceholderText(tr("Find or create a room"));
    m_filter->setAccessibleName(tr("Find or create a room"));
    a11y::addClearButton(m_filter);
    connect(m_filter, &QLineEdit::returnPressed, this, &RoomsView::joinTyped);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_listProxy->setFilterFixedString(text.trimmed());
        m_joinProblem->hide();
    });
    m_listModel->setHorizontalHeaderLabels({tr("Room"), tr("Users")});
    m_listModel->setSortRole(kSortRole);
    m_listProxy->setSourceModel(m_listModel);
    // The proxy has its columns only now.
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_listProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_listProxy->setFilterKeyColumn(0);
    m_listProxy->setSortRole(kSortRole);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(1, Qt::DescendingOrder); // the busiest first
    connect(m_list, &QTreeView::doubleClicked, this, &RoomsView::joinSelected);
    a11y::onReturn(m_list, [this] { joinSelected(); });
    m_list->setAccessibleName(tr("All rooms"));
    m_join->setObjectName(u"rooms.join"_s);
    m_join->setToolTip(tr("Join the room picked in the list, or the name typed above"));
    connect(m_join, &QPushButton::clicked, this, [this] {
        if (m_list->currentIndex().isValid()) {
            joinSelected();
        } else {
            joinTyped();
        }
    });
    m_joinProblem->setObjectName(u"rooms.joinProblem"_s);
    m_joinProblem->setWordWrap(true);
    m_joinProblem->setForegroundRole(QPalette::PlaceholderText);
    m_joinProblem->hide();

    auto *left = new QWidget(this);
    auto *layout = new QVBoxLayout(left);
    layout->setContentsMargins(12, 10, 0, 8);
    layout->setSpacing(6);
    layout->addWidget(heading(tr("Your rooms"), left));
    layout->addWidget(m_joined, 1);
    layout->addWidget(heading(tr("All rooms"), left));
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 3);
    layout->addWidget(m_joinProblem);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_join);
    layout->addLayout(buttons);
    return left;
}

QWidget *RoomsView::createRoomPage()
{
    QFont nameFont = m_name->font();
    nameFont.setWeight(QFont::DemiBold);
    nameFont.setPointSizeF(nameFont.pointSizeF() + 2.0);
    m_name->setFont(nameFont);
    m_name->setObjectName(u"rooms.name"_s);
    m_details->setObjectName(u"rooms.details"_s);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    m_details->setWordWrap(true);
    m_leave->setObjectName(u"rooms.leave"_s);
    connect(m_leave, &QPushButton::clicked, this, [this] {
        if (!m_room.isEmpty()) {
            m_engine.leaveRoom(m_room);
        }
    });

    m_lines->setObjectName(u"rooms.lines"_s);
    m_lines->setFrameShape(QFrame::NoFrame);
    m_lines->setOpenLinks(false); // lines are shown as typed
    m_lines->setAccessibleName(tr("Conversation"));
    m_lines->document()->setMaximumBlockCount(core::RoomService::kKeptLines);
    m_members->setObjectName(u"rooms.members"_s);
    m_members->setModel(m_membersModel);
    m_members->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_members->setFrameShape(QFrame::NoFrame);
    m_members->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_members->setUniformItemSizes(true);
    m_members->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_members, &QListView::customContextMenuRequested, this, &RoomsView::membersMenu);
    connect(m_members, &QListView::doubleClicked, this,
            [this](const QModelIndex &index) { Q_EMIT messageRequested(index.data().toString()); });
    a11y::onReturn(m_members, [this] {
        if (const QModelIndex current = m_members->currentIndex(); current.isValid()) {
            Q_EMIT messageRequested(current.data().toString());
        }
    });
    m_members->setAccessibleName(tr("Members"));
    a11y::addMenuKey(m_members);

    m_problem->setObjectName(u"rooms.problem"_s);
    m_problem->setWordWrap(true);
    m_problem->setForegroundRole(QPalette::PlaceholderText);
    m_problem->hide();
    m_input->setObjectName(u"rooms.input"_s);
    m_input->setMaxLength(core::SocialService::kMaxTextLength);
    m_input->setAccessibleName(tr("Your message"));
    m_send->setObjectName(u"rooms.send"_s);
    connect(m_input, &QLineEdit::returnPressed, this, &RoomsView::post);
    connect(m_input, &QLineEdit::textChanged, this, [this] { m_problem->hide(); });
    connect(m_send, &QPushButton::clicked, this, &RoomsView::post);

    auto *nameBox = new QVBoxLayout;
    nameBox->setSpacing(0);
    nameBox->addWidget(m_name);
    nameBox->addWidget(m_details);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(16, 10, 16, 8);
    top->addLayout(nameBox, 1);
    top->addWidget(m_leave, 0, Qt::AlignTop);
    auto *talk = new QSplitter(Qt::Horizontal, this);
    talk->setObjectName(u"rooms.talk"_s);
    talk->setChildrenCollapsible(false);
    talk->addWidget(m_lines);
    talk->addWidget(m_members);
    talk->setStretchFactor(0, 1);
    talk->setSizes({520, 160});
    auto *problemBox = new QHBoxLayout;
    problemBox->setContentsMargins(16, 0, 16, 0);
    problemBox->addWidget(m_problem);
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(16, 8, 16, 12);
    bottom->addWidget(m_input, 1);
    bottom->addWidget(m_send);

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(talk, 1);
    layout->addLayout(problemBox);
    layout->addLayout(bottom);
    return page;
}

void RoomsView::openRoom(const QString &room)
{
    if (!m_rooms.contains(room)) {
        return;
    }
    if (room != m_room) {
        m_room = room;
        m_problem->hide();
        m_input->clear();
        renderLines();
    }
    refreshRoom();
    rebuildJoined();
    updateViewing();
    m_input->setFocus();
    Q_EMIT summaryChanged();
}

QString RoomsView::summary() const
{
    if (m_rooms.isEmpty()) {
        return m_engine.networkStatus().state == core::NetworkStatus::State::Online ? tr("No rooms joined")
                                                                                    : tr("Not connected");
    }
    const QString count = m_rooms.size() == 1 ? tr("In 1 room") : tr("In %1 rooms").arg(m_rooms.size());
    const int waiting = unread();
    return waiting > 0 ? tr("%1 · %2 unread").arg(count).arg(waiting) : count;
}

int RoomsView::unread() const
{
    int unread = 0;
    for (const core::Room &room : m_rooms) {
        unread += room.unread;
    }
    return unread;
}

bool RoomsView::isReading(const QString &room) const
{
    return !room.isEmpty() && room == m_room && isVisible() && window()->isActiveWindow();
}

void RoomsView::rebuildJoined()
{
    const QSignalBlocker blocker(m_joined->selectionModel()); // rebuilding isn't the user picking
    m_joinedModel->removeRows(0, m_joinedModel->rowCount());
    for (const core::Room &room : std::as_const(m_rooms)) {
        auto *name = new QStandardItem(room.name);
        name->setData(room.name, kNameRole);
        name->setToolTip(stateText(room));
        QString count;
        if (room.state == core::Room::State::Joined) {
            count = room.unread > 0 ? QLocale().toString(room.unread) : QString();
        } else {
            count = room.state == core::Room::State::Failed ? tr("Refused") : tr("Joining");
        }
        auto *unread = new QStandardItem(room.mentions > 0 ? tr("@ %1").arg(count) : count);
        if (room.state == core::Room::State::Joined) {
            QString words = room.unread > 0 ? tr("%1 unread").arg(count) : tr("Nothing unread");
            if (room.mentions > 0) {
                words += tr(", and you were mentioned");
            }
            unread->setData(words, Qt::AccessibleTextRole);
        }
        unread->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (room.unread > 0) {
            QFont bold = name->font();
            bold.setBold(true);
            name->setFont(bold);
            unread->setFont(bold);
        } else {
            unread->setForeground(QPalette().brush(QPalette::PlaceholderText));
        }
        m_joinedModel->appendRow({name, unread});
        if (room.name == m_room) {
            m_joined->setCurrentIndex(name->index());
        }
    }
}

void RoomsView::rebuildList(const QList<core::RoomListing> &rooms)
{
    m_listings = rooms;
    m_listModel->removeRows(0, m_listModel->rowCount());
    for (const core::RoomListing &listing : rooms) {
        auto *name = new QStandardItem(listing.name);
        name->setData(listing.name, kNameRole);
        name->setData(listing.name.toCaseFolded(), kSortRole);
        auto *users = new QStandardItem(QLocale().toString(listing.users));
        users->setData(listing.users, kSortRole);
        users->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        users->setForeground(QPalette().brush(QPalette::PlaceholderText));
        m_listModel->appendRow({name, users});
    }
    m_list->sortByColumn(m_list->header()->sortIndicatorSection(), m_list->header()->sortIndicatorOrder());
}

void RoomsView::refreshRoom()
{
    const auto it = m_rooms.constFind(m_room);
    m_pages->setCurrentIndex(it == m_rooms.constEnd() ? 0 : 1);
    if (it == m_rooms.constEnd()) {
        m_membersModel->setStringList({});
        return;
    }
    m_name->setText(it->name);
    m_details->setText(stateText(*it));
    if (m_membersModel->stringList() != it->members) {
        m_membersModel->setStringList(it->members);
    }
    const bool joined = it->state == core::Room::State::Joined;
    const bool online = m_engine.networkStatus().state == core::NetworkStatus::State::Online;
    m_input->setEnabled(joined && online);
    m_send->setEnabled(joined && online);
    if (!online) {
        m_input->setPlaceholderText(tr("Not connected to Soulseek"));
    } else {
        m_input->setPlaceholderText(joined ? tr("Say something in %1").arg(it->name) : tr("Not in the room yet"));
    }
    m_leave->setText(it->state == core::Room::State::Failed ? tr("Close") : tr("Leave"));
}

void RoomsView::renderLines()
{
    m_lines->clear();
    const QList<core::RoomLine> lines = m_engine.roomLines(m_room);
    for (const core::RoomLine &line : lines) {
        appendLine(line);
    }
}

void RoomsView::appendLine(const core::RoomLine &line)
{
    const QString meta = palette().color(QPalette::PlaceholderText).name();
    const QString time = QLocale().toString(QDateTime::fromMSecsSinceEpoch(line.atMs).time(), QLocale::ShortFormat);
    const QString who =
        line.own ? u"<b style=\"color:%1\">%2</b>"_s.arg(palette().color(QPalette::Active, QPalette::Accent).name(),
                                                         line.user.toHtmlEscaped())
                 : u"<b>%1</b>"_s.arg(line.user.toHtmlEscaped());
    const QString text = line.mention ? u"<b>%1</b>"_s.arg(line.text.toHtmlEscaped()) : line.text.toHtmlEscaped();
    QScrollBar *bar = m_lines->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 4;
    m_lines->append(u"<span style=\"color:%1\">%2</span>&nbsp; %3&nbsp; %4"_s.arg(meta, time, who, text));
    if (atEnd) {
        bar->setValue(bar->maximum()); // follows the conversation unless the user scrolled back
    }
}

void RoomsView::joinTyped()
{
    const QString name = m_filter->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    // A name in the list joins that room, whatever its case.
    for (const core::RoomListing &listing : std::as_const(m_listings)) {
        if (listing.name.compare(name, Qt::CaseInsensitive) == 0) {
            join(listing.name);
            return;
        }
    }
    join(name);
}

void RoomsView::joinSelected()
{
    const QString name = m_list->currentIndex().siblingAtColumn(0).data(kNameRole).toString();
    if (!name.isEmpty()) {
        join(name);
    }
}

void RoomsView::join(const QString &room)
{
    m_joinProblem->hide();
    if (m_rooms.contains(room)) {
        openRoom(room);
        return;
    }
    const bool listed =
        std::ranges::any_of(m_listings, [&room](const core::RoomListing &listing) { return listing.name == room; });
    if (listed) {
        m_engine.joinRoom(room);
        m_filter->clear();
        return;
    }
    // Joining a name nobody uses creates a public room: ask first.
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setObjectName(u"rooms.createConfirm"_s);
    box->setIcon(QMessageBox::Question);
    box->setText(tr("Create the room “%1”?").arg(room));
    box->setInformativeText(tr("No room by that name is in the server's list. Joining it creates a public room that "
                               "anyone on Soulseek can find and join."));
    QPushButton *create = box->addButton(tr("Create Room"), QMessageBox::AcceptRole);
    create->setObjectName(u"rooms.createConfirm.create"_s);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(create);
    box->setWindowModality(Qt::WindowModal); // a sheet on macOS
    connect(box, &QMessageBox::finished, this, [this, box, create, room] {
        if (box->clickedButton() == create) {
            m_engine.joinRoom(room);
            m_filter->clear();
        }
    });
    box->open();
}

void RoomsView::post()
{
    const QString text = m_input->text().trimmed();
    if (m_room.isEmpty() || text.isEmpty() || !m_send->isEnabled()) {
        return;
    }
    m_send->setEnabled(false); // until the engine says how it went
    m_engine.sayInRoom(m_room, text);
}

void RoomsView::membersMenu(QPoint position)
{
    const QString peer = m_members->indexAt(position).data().toString();
    if (peer.isEmpty()) {
        return;
    }
    QMenu menu(this);
    PeerMenu::addActions(menu, peer, m_engine,
                         {.browse = [this](const QString &name) { Q_EMIT browseRequested(name); },
                          .message = [this](const QString &name) { Q_EMIT messageRequested(name); }});
    menu.exec(m_members->viewport()->mapToGlobal(position));
}

void RoomsView::updateViewing()
{
    const QString viewing = isReading(m_room) ? m_room : QString();
    if (viewing != m_viewing) {
        m_viewing = viewing;
        m_engine.setViewingRoom(viewing);
    }
}

void RoomsView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_engine.refreshRoomList(); // at most once a minute: the engine keeps to it
    updateViewing();
}

void RoomsView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    updateViewing();
}

} // namespace selecta::app
