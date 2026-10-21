// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/MessagesView.h"

#include "app/Accessibility.h"
#include "app/Format.h"
#include "app/PeerMenu.h"
#include "core/EngineHost.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kPeerRole = Qt::UserRole + 1;
constexpr int kSortRole = Qt::UserRole + 2;
enum ConversationColumn { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
    PeerColumn,
    WhenColumn,
    ConversationColumnCount
};
enum PeopleColumn { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
    UserColumn,
    ListColumn,
    StatusColumn,
    AddedColumn,
    PeopleColumnCount
};

QString listTitle(core::UserList list)
{
    switch (list) {
    case core::UserList::Buddy:
        return MessagesView::tr("Buddy");
    case core::UserList::Ignored:
        return MessagesView::tr("Ignored");
    case core::UserList::Banned:
        return MessagesView::tr("Banned");
    }
    return {};
}

QString presenceTitle(core::Presence presence)
{
    switch (presence) {
    case core::Presence::Online:
        return MessagesView::tr("Online");
    case core::Presence::Away:
        return MessagesView::tr("Away");
    case core::Presence::Offline:
        return MessagesView::tr("Offline");
    case core::Presence::Unknown:
        break;
    }
    return {};
}

QTreeView *makeTable(const QString &name, QStandardItemModel *model, QWidget *parent)
{
    auto *table = new QTreeView(parent);
    table->setObjectName(name);
    table->setModel(model);
    table->setRootIsDecorated(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFrameShape(QFrame::NoFrame);
    table->setAttribute(Qt::WA_MacShowFocusRect, false);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    table->header()->setStretchLastSection(false);
    return table;
}

} // namespace

MessagesView::MessagesView(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_pages(new QTabWidget(this))
    , m_to(new QLineEdit(this))
    , m_conversations(new QStandardItemModel(0, ConversationColumnCount, this))
    , m_list(makeTable(u"messages.conversations"_s, m_conversations, this))
    , m_threadPages(new QStackedWidget(this))
    , m_peerName(new QLabel(this))
    , m_peerState(new QLabel(this))
    , m_browse(new QPushButton(tr("Browse Files"), this))
    , m_buddy(new QPushButton(this))
    , m_more(new QPushButton(tr("More"), this))
    , m_thread(new QTextBrowser(this))
    , m_problem(new QLabel(this))
    , m_input(new QLineEdit(this))
    , m_send(new QPushButton(tr("Send"), this))
    , m_person(new QLineEdit(this))
    , m_listChoice(new QComboBox(this))
    , m_people(new QStandardItemModel(0, PeopleColumnCount, this))
    , m_peopleTable(makeTable(u"people.table"_s, m_people, this))
{
    setObjectName(u"view.messages"_s);
    m_pages->setObjectName(u"messages.pages"_s);
    m_pages->setDocumentMode(true);
    m_pages->addTab(createConversationsPage(), tr("Conversations"));
    m_pages->addTab(createPeoplePage(), tr("People"));
    connect(m_pages, &QTabWidget::currentChanged, this, [this] {
        markOpenRead();
        Q_EMIT summaryChanged();
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_pages);

    connect(&m_engine, &core::EngineHost::conversationsChanged, this,
            [this](const QList<core::Conversation> &conversations) {
                m_conversationList = conversations;
                rebuildConversations();
                Q_EMIT summaryChanged();
            });
    connect(&m_engine, &core::EngineHost::conversationLoaded, this,
            [this](const QString &peer, const QList<core::ChatMessage> &messages) {
                if (peer == m_peer) {
                    m_messages = messages;
                    renderThread();
                }
            });
    connect(&m_engine, &core::EngineHost::messageAdded, this, [this](const core::ChatMessage &message) {
        if (message.peer != m_peer) {
            return;
        }
        m_messages.append(message);
        renderThread();
        if (!message.outgoing) {
            markOpenRead();
        }
    });
    connect(&m_engine, &core::EngineHost::messageSent, this, &MessagesView::sent);
    connect(&m_engine, &core::EngineHost::userListsChanged, this, [this](const QList<core::ListedUser> &users) {
        m_users = users;
        rebuildPeople();
        refreshThreadBar();
    });
    connect(&m_engine, &core::EngineHost::networkStatusChanged, this, &MessagesView::refreshThreadBar);
    // Coming back to the window reads the conversation it shows.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            markOpenRead();
        }
    });
    m_conversationList = m_engine.conversations();
    m_users = m_engine.userLists();
    rebuildConversations();
    rebuildPeople();
    refreshThreadBar();
}

QWidget *MessagesView::createConversationsPage()
{
    m_to->setObjectName(u"messages.to"_s);
    m_to->setPlaceholderText(tr("Message a user…"));
    m_to->setAccessibleName(tr("Message a user"));
    a11y::addClearButton(m_to);
    m_to->setToolTip(tr("Type a Soulseek username and press Return to write to them"));
    connect(m_to, &QLineEdit::returnPressed, this, &MessagesView::startTyped);

    m_conversations->setHorizontalHeaderLabels({tr("Conversation"), tr("When")});
    m_list->setHeaderHidden(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->header()->setSectionResizeMode(PeerColumn, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(WhenColumn, QHeaderView::ResizeToContents);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
        const QString peer = current.siblingAtColumn(PeerColumn).data(kPeerRole).toString();
        if (!peer.isEmpty() && peer != m_peer) {
            openConversation(peer);
        }
    });
    connect(m_list, &QTreeView::customContextMenuRequested, this, &MessagesView::conversationsMenu);
    m_list->setAccessibleName(tr("Conversations"));
    a11y::addMenuKey(m_list);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(12, 10, 0, 0);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(m_to);
    leftLayout->addWidget(m_list, 1);

    auto *empty = new QLabel(tr("No conversation open. Pick one, or type a username above to write to them."), this);
    empty->setObjectName(u"messages.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_threadPages->addWidget(empty);
    m_threadPages->addWidget(createThread());

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(u"messages.splitter"_s);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(left);
    splitter->addWidget(m_threadPages);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 600});
    return splitter;
}

QWidget *MessagesView::createThread()
{
    QFont nameFont = m_peerName->font();
    nameFont.setWeight(QFont::DemiBold);
    nameFont.setPointSizeF(nameFont.pointSizeF() + 2.0);
    m_peerName->setFont(nameFont);
    m_peerName->setObjectName(u"messages.peer"_s);
    m_peerName->setTextFormat(Qt::PlainText); // a peer can be named anything
    m_peerState->setObjectName(u"messages.peerState"_s);
    m_peerState->setTextFormat(Qt::PlainText);
    m_peerState->setForegroundRole(QPalette::PlaceholderText);
    m_browse->setObjectName(u"messages.browse"_s);
    m_buddy->setObjectName(u"messages.buddy"_s);
    m_more->setObjectName(u"messages.more"_s);
    auto *moreMenu = new QMenu(m_more); // a pull-down button on macOS
    m_more->setMenu(moreMenu);
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
        moreMenu->clear();
        const std::optional<core::ListedUser> user = listedUser(m_peer);
        const std::optional<core::UserList> listed = user ? std::optional(user->list) : std::nullopt;
        moreMenu->addAction(
            listed == core::UserList::Ignored ? tr("Stop Ignoring") : tr("Ignore Messages"), this, [this, listed] {
                m_engine.setUserListed(
                    m_peer, listed == core::UserList::Ignored ? std::nullopt : std::optional(core::UserList::Ignored));
            });
        moreMenu->addAction(
            listed == core::UserList::Banned ? tr("Unban") : tr("Ban from Your Shares"), this, [this, listed] {
                m_engine.setUserListed(
                    m_peer, listed == core::UserList::Banned ? std::nullopt : std::optional(core::UserList::Banned));
            });
        moreMenu->addSeparator();
        moreMenu->addAction(tr("Delete Conversation…"), this, [this] {
            auto *box = new QMessageBox(this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setObjectName(u"messages.deleteConfirm"_s);
            box->setIcon(QMessageBox::Warning);
            box->setTextFormat(Qt::PlainText); // a peer's name
            box->setText(tr("Delete the conversation with %1?").arg(m_peer));
            box->setInformativeText(tr("Its messages are removed from this Mac. This can't be undone."));
            QPushButton *remove = box->addButton(tr("Delete"), QMessageBox::DestructiveRole);
            remove->setObjectName(u"messages.deleteConfirm.delete"_s);
            box->addButton(QMessageBox::Cancel);
            box->setWindowModality(Qt::WindowModal); // a sheet on macOS
            connect(box, &QMessageBox::finished, this, [this, box, remove, peer = m_peer] {
                if (box->clickedButton() == remove) {
                    m_engine.deleteConversation(peer);
                    if (peer == m_peer) {
                        m_peer.clear();
                        m_messages.clear();
                        refreshThreadBar();
                    }
                }
            });
            box->open();
        });
    });
    connect(m_browse, &QPushButton::clicked, this, [this] { Q_EMIT browseRequested(m_peer); });
    connect(m_buddy, &QPushButton::clicked, this, [this] {
        const std::optional<core::ListedUser> user = listedUser(m_peer);
        const bool buddy = user && user->list == core::UserList::Buddy;
        m_engine.setUserListed(m_peer, buddy ? std::nullopt : std::optional(core::UserList::Buddy));
    });

    m_thread->setObjectName(u"messages.thread"_s);
    m_thread->setFrameShape(QFrame::NoFrame);
    m_thread->setOpenLinks(false); // messages are shown as typed, links included
    m_thread->setAccessibleName(tr("Messages"));
    m_problem->setObjectName(u"messages.problem"_s);
    m_problem->setTextFormat(Qt::PlainText); // may be the server's words
    m_problem->setWordWrap(true);
    m_problem->setForegroundRole(QPalette::PlaceholderText);
    m_problem->hide();
    m_input->setObjectName(u"messages.input"_s);
    m_input->setMaxLength(core::SocialService::kMaxTextLength);
    m_input->setAccessibleName(tr("Your message"));
    m_input->installEventFilter(this);
    m_send->setObjectName(u"messages.send"_s);
    connect(m_input, &QLineEdit::returnPressed, this, &MessagesView::send);
    connect(m_input, &QLineEdit::textChanged, this, [this] { m_problem->hide(); });
    connect(m_send, &QPushButton::clicked, this, &MessagesView::send);

    auto *nameBox = new QVBoxLayout;
    nameBox->setSpacing(0);
    nameBox->addWidget(m_peerName);
    nameBox->addWidget(m_peerState);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(16, 10, 16, 8);
    top->addLayout(nameBox, 1);
    top->addWidget(m_browse);
    top->addWidget(m_buddy);
    top->addWidget(m_more);
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(16, 8, 16, 12);
    bottom->addWidget(m_input, 1);
    bottom->addWidget(m_send);
    auto *problemBox = new QHBoxLayout;
    problemBox->setContentsMargins(16, 0, 16, 0);
    problemBox->addWidget(m_problem);

    auto *thread = new QWidget(this);
    auto *layout = new QVBoxLayout(thread);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(m_thread, 1);
    layout->addLayout(problemBox);
    layout->addLayout(bottom);
    return thread;
}

QWidget *MessagesView::createPeoplePage()
{
    m_person->setObjectName(u"people.user"_s);
    m_person->setPlaceholderText(tr("Soulseek username"));
    m_person->setAccessibleName(tr("Soulseek username"));
    a11y::addClearButton(m_person);
    m_listChoice->setObjectName(u"people.list"_s);
    m_listChoice->setAccessibleName(tr("List to add them to"));
    m_listChoice->addItem(tr("Buddies"), static_cast<int>(core::UserList::Buddy));
    m_listChoice->addItem(tr("Ignored"), static_cast<int>(core::UserList::Ignored));
    m_listChoice->addItem(tr("Banned"), static_cast<int>(core::UserList::Banned));
    auto *add = new QPushButton(tr("Add"), this);
    add->setObjectName(u"people.add"_s);
    connect(m_person, &QLineEdit::returnPressed, this, &MessagesView::addPerson);
    connect(add, &QPushButton::clicked, this, &MessagesView::addPerson);
    auto *note = new QLabel(tr("Buddies are watched while you're online, so you see when they are too. Messages from "
                               "ignored users are dropped unread. Banned users get nothing of your shares: no search "
                               "results, no browsing, no downloads."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    m_people->setHorizontalHeaderLabels({tr("User"), tr("List"), tr("Status"), tr("Added")});
    m_people->setSortRole(kSortRole);
    m_peopleTable->setUniformRowHeights(true);
    m_peopleTable->setAlternatingRowColors(true);
    m_peopleTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_peopleTable->setSortingEnabled(true);
    m_peopleTable->sortByColumn(UserColumn, Qt::AscendingOrder);
    m_peopleTable->header()->setSectionResizeMode(UserColumn, QHeaderView::Stretch);
    m_peopleTable->setColumnWidth(ListColumn, 110);
    m_peopleTable->setColumnWidth(StatusColumn, 110);
    m_peopleTable->setColumnWidth(AddedColumn, 140);
    connect(m_peopleTable, &QTreeView::customContextMenuRequested, this, &MessagesView::peopleMenu);
    m_peopleTable->setAccessibleName(tr("People"));
    m_peopleTable->installEventFilter(this); // Return opens the conversation, as a double-click does
    a11y::addMenuKey(m_peopleTable);
    connect(m_peopleTable, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString peer = index.siblingAtColumn(UserColumn).data(kPeerRole).toString();
        if (!peer.isEmpty()) {
            openConversation(peer);
        }
    });

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(16, 10, 16, 4);
    bar->addWidget(m_person, 1);
    bar->addWidget(m_listChoice);
    bar->addWidget(add);
    auto *noteBox = new QHBoxLayout;
    noteBox->setContentsMargins(16, 0, 16, 8);
    noteBox->addWidget(note);
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addLayout(noteBox);
    layout->addWidget(m_peopleTable, 1);
    return page;
}

void MessagesView::openConversation(const QString &peer)
{
    const QString name = peer.trimmed();
    if (name.isEmpty()) {
        return;
    }
    m_pages->setCurrentIndex(static_cast<int>(Page::Conversations));
    if (name != m_peer) {
        m_peer = name;
        m_messages.clear();
        m_problem->hide();
        m_input->clear();
        m_engine.loadConversation(name);
        rebuildConversations(); // a new conversation shows at the top until it has messages
    }
    renderThread();
    refreshThreadBar();
    markOpenRead();
    m_input->setFocus();
    Q_EMIT summaryChanged();
}

void MessagesView::showPage(Page page)
{
    m_pages->setCurrentIndex(static_cast<int>(page));
}

bool MessagesView::isReading(const QString &peer) const
{
    return !peer.isEmpty() && peer == m_peer && isVisible() && window()->isActiveWindow() &&
           m_pages->currentIndex() == static_cast<int>(Page::Conversations);
}

QString MessagesView::summary() const
{
    if (m_pages->currentIndex() == static_cast<int>(Page::People)) {
        const qsizetype people = m_users.size();
        if (people == 0) {
            return tr("Nobody on your lists");
        }
        return people == 1 ? tr("1 person on your lists") : tr("%1 people on your lists").arg(people);
    }
    const qsizetype conversations = m_conversationList.size();
    if (conversations == 0) {
        return tr("No conversations");
    }
    const QString count = conversations == 1 ? tr("1 conversation") : tr("%1 conversations").arg(conversations);
    const int waiting = unread();
    return waiting > 0 ? tr("%1 · %2 unread").arg(count).arg(waiting) : count;
}

int MessagesView::unread() const
{
    int unread = 0;
    for (const core::Conversation &conversation : m_conversationList) {
        unread += conversation.unread;
    }
    return unread;
}

std::optional<core::ListedUser> MessagesView::listedUser(const QString &peer) const
{
    for (const core::ListedUser &user : m_users) {
        if (user.username == peer) {
            return user;
        }
    }
    return std::nullopt;
}

void MessagesView::rebuildConversations()
{
    const QList<core::Conversation> &conversations = m_conversationList;
    const QSignalBlocker blocker(m_list->selectionModel()); // rebuilding isn't the user picking
    m_conversations->removeRows(0, m_conversations->rowCount());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto addRow = [this](const QString &peer, const QString &line, const QString &when, int unread) {
        auto *who = new QStandardItem(line.isEmpty() ? peer : peer + u'\n' + line);
        who->setData(peer, kPeerRole);
        who->setToolTip(format::plainToolTip(peer));
        auto *at = new QStandardItem(unread > 0 ? when + u'\n' + tr("%n new", nullptr, unread) : when);
        at->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        at->setForeground(QPalette().brush(QPalette::PlaceholderText));
        if (unread > 0) {
            QFont bold = who->font();
            bold.setBold(true);
            who->setFont(bold);
            at->setFont(bold);
        }
        m_conversations->appendRow({who, at});
    };
    bool openListed = m_peer.isEmpty();
    for (const core::Conversation &conversation : conversations) {
        openListed = openListed || conversation.peer == m_peer;
    }
    if (!openListed) {
        addRow(m_peer, tr("New conversation"), QString(), 0);
    }
    for (const core::Conversation &conversation : conversations) {
        const QString line =
            conversation.lastOutgoing ? tr("You: %1").arg(conversation.lastText) : conversation.lastText;
        addRow(conversation.peer, line, format::ago(conversation.lastAtMs, now), conversation.unread);
    }
    for (int row = 0; row < m_conversations->rowCount(); ++row) {
        if (m_conversations->item(row, PeerColumn)->data(kPeerRole).toString() == m_peer) {
            m_list->setCurrentIndex(m_conversations->index(row, PeerColumn));
            break;
        }
    }
}

void MessagesView::rebuildPeople()
{
    const QStringList selected = selectedPeople();
    m_people->removeRows(0, m_people->rowCount());
    const QLocale locale;
    for (const core::ListedUser &user : std::as_const(m_users)) {
        auto *name = new QStandardItem(user.username);
        name->setData(user.username, kPeerRole);
        name->setData(user.username.toCaseFolded(), kSortRole);
        auto *list = new QStandardItem(listTitle(user.list));
        list->setData(static_cast<int>(user.list), kSortRole);
        auto *status = new QStandardItem(user.list == core::UserList::Buddy ? presenceTitle(user.presence) : QString());
        status->setData(-static_cast<int>(user.presence), kSortRole); // online first
        if (user.presence != core::Presence::Online) {
            status->setForeground(QPalette().brush(QPalette::PlaceholderText));
        }
        auto *added = new QStandardItem(
            locale.toString(QDateTime::fromMSecsSinceEpoch(user.addedMs).date(), QLocale::ShortFormat));
        added->setData(user.addedMs, kSortRole);
        added->setForeground(QPalette().brush(QPalette::PlaceholderText));
        m_people->appendRow({name, list, status, added});
    }
    m_peopleTable->sortByColumn(m_peopleTable->header()->sortIndicatorSection(),
                                m_peopleTable->header()->sortIndicatorOrder());
    for (int row = 0; row < m_people->rowCount(); ++row) {
        if (selected.contains(m_people->item(row, UserColumn)->text())) {
            m_peopleTable->selectionModel()->select(m_people->index(row, UserColumn),
                                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    if (m_pages->currentIndex() == static_cast<int>(Page::People)) {
        Q_EMIT summaryChanged();
    }
}

void MessagesView::renderThread()
{
    if (m_peer.isEmpty()) {
        m_thread->clear();
        return;
    }
    const QString meta = palette().color(QPalette::PlaceholderText).name();
    if (m_messages.isEmpty()) {
        m_thread->setHtml(u"<p align=\"center\" style=\"color:%1; margin-top:24px\">%2</p>"_s.arg(
            meta, tr("No messages with %1 yet. What you write goes through the Soulseek server, paced like "
                     "everything Selecta sends.")
                      .arg(m_peer)
                      .toHtmlEscaped()));
        return;
    }
    const QLocale locale;
    QString html;
    QDate day;
    for (const core::ChatMessage &message : std::as_const(m_messages)) {
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(message.atMs);
        if (at.date() != day) {
            day = at.date();
            html += u"<p align=\"center\" style=\"color:%1; margin-top:14px\">%2</p>"_s.arg(
                meta, locale.toString(day, QLocale::LongFormat).toHtmlEscaped());
        }
        const QString who = message.outgoing ? tr("You") : message.peer;
        html += u"<p align=\"%1\" style=\"margin-top:8px; margin-bottom:0\"><span style=\"color:%2\">%3 · %4</span>"
                "<br/>%5</p>"_s.arg(message.outgoing ? u"right"_s : u"left"_s, meta, who.toHtmlEscaped(),
                                    locale.toString(at.time(), QLocale::ShortFormat), message.text.toHtmlEscaped());
    }
    m_thread->setHtml(html);
    m_thread->moveCursor(QTextCursor::End);
    m_thread->ensureCursorVisible();
}

void MessagesView::refreshThreadBar()
{
    m_threadPages->setCurrentIndex(m_peer.isEmpty() ? 0 : 1);
    if (m_peer.isEmpty()) {
        return;
    }
    m_peerName->setText(m_peer);
    const std::optional<core::ListedUser> listed = listedUser(m_peer);
    QString state;
    if (listed && listed->list == core::UserList::Buddy) {
        const QString presence = presenceTitle(listed->presence);
        state = presence.isEmpty() ? tr("Buddy") : tr("Buddy · %1").arg(presence);
    } else if (listed && listed->list == core::UserList::Ignored) {
        state = tr("Ignored: their messages are dropped unread");
    } else if (listed && listed->list == core::UserList::Banned) {
        state = tr("Banned from your shares");
    }
    m_peerState->setText(state);
    m_peerState->setVisible(!state.isEmpty());
    m_buddy->setText(listed && listed->list == core::UserList::Buddy ? tr("Remove from Buddies")
                                                                     : tr("Add to Buddies"));
    m_thread->setAccessibleName(tr("Messages with %1").arg(m_peer));
    const bool online = m_engine.networkStatus().state == core::NetworkStatus::State::Online;
    m_input->setPlaceholderText(online ? tr("Message %1").arg(m_peer) : tr("Not connected to Soulseek"));
    m_send->setEnabled(online && !m_sending);
}

void MessagesView::send()
{
    const QString text = m_input->text().trimmed();
    if (m_peer.isEmpty() || text.isEmpty() || m_sending) {
        return;
    }
    if (m_engine.networkStatus().state != core::NetworkStatus::State::Online) {
        m_problem->setText(tr("Not connected to Soulseek."));
        m_problem->show();
        return;
    }
    m_sending = true;
    m_send->setEnabled(false);
    m_engine.sendMessage(m_peer, text);
}

void MessagesView::sent(const QString &peer, const QString &problem)
{
    m_sending = false;
    refreshThreadBar();
    if (peer != m_peer) {
        return;
    }
    if (problem.isEmpty()) {
        m_input->clear();
        m_problem->hide();
    } else {
        m_problem->setText(problem);
        m_problem->show();
    }
}

void MessagesView::markOpenRead()
{
    if (!isReading(m_peer)) {
        return;
    }
    for (const core::Conversation &conversation : std::as_const(m_conversationList)) {
        if (conversation.peer == m_peer && conversation.unread > 0) {
            m_engine.markConversationRead(m_peer);
            return;
        }
    }
}

void MessagesView::startTyped()
{
    const QString name = m_to->text().trimmed();
    if (!name.isEmpty()) {
        m_to->clear();
        openConversation(name);
    }
}

void MessagesView::addPerson()
{
    const QString name = m_person->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    m_engine.setUserListed(name, static_cast<core::UserList>(m_listChoice->currentData().toInt()));
    m_person->clear();
}

QStringList MessagesView::selectedPeople() const
{
    QStringList people;
    const QModelIndexList rows = m_peopleTable->selectionModel()->selectedRows(UserColumn);
    for (const QModelIndex &row : rows) {
        people.append(row.data(kPeerRole).toString());
    }
    return people;
}

void MessagesView::peopleMenu(QPoint position)
{
    const QStringList people = selectedPeople();
    if (people.isEmpty()) {
        return;
    }
    QMenu menu(this);
    if (people.size() == 1) {
        PeerMenu::addActions(menu, people.constFirst(), m_engine,
                             {.browse = [this](const QString &peer) { Q_EMIT browseRequested(peer); },
                              .message = [this](const QString &peer) { openConversation(peer); }});
        menu.addSeparator();
    }
    const auto move = [this, people](std::optional<core::UserList> list) {
        for (const QString &peer : people) {
            m_engine.setUserListed(peer, list);
        }
    };
    if (people.size() > 1) {
        menu.addAction(tr("Move to Buddies"), this, [move] { move(core::UserList::Buddy); });
        menu.addAction(tr("Move to Ignored"), this, [move] { move(core::UserList::Ignored); });
        menu.addAction(tr("Move to Banned"), this, [move] { move(core::UserList::Banned); });
    }
    menu.addAction(people.size() == 1 ? tr("Remove from Lists") : tr("Remove %1 from Lists").arg(people.size()), this,
                   [move] { move(std::nullopt); });
    menu.exec(m_peopleTable->viewport()->mapToGlobal(position));
}

void MessagesView::conversationsMenu(QPoint position)
{
    const QString peer = m_list->indexAt(position).siblingAtColumn(PeerColumn).data(kPeerRole).toString();
    if (peer.isEmpty()) {
        return;
    }
    QMenu menu(this);
    PeerMenu::addActions(menu, peer, m_engine,
                         {.browse = [this](const QString &name) { Q_EMIT browseRequested(name); }, .message = {}});
    menu.exec(m_list->viewport()->mapToGlobal(position));
}

void MessagesView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    markOpenRead();
    // The thread was laid out while hidden: show its latest messages.
    QTimer::singleShot(0, this, [this] {
        m_thread->moveCursor(QTextCursor::End);
        m_thread->ensureCursorVisible();
    });
}

bool MessagesView::eventFilter(QObject *watched, QEvent *event)
{
    // Escape in the reply field leaves it for the list; Return in People opens the conversation.
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        if (key && key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            m_list->setFocus();
            return true;
        }
    }
    if (watched == m_peopleTable && event->type() == QEvent::KeyPress) {
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        if (key && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::NoModifier) {
            const QString peer = m_peopleTable->currentIndex().siblingAtColumn(UserColumn).data(kPeerRole).toString();
            if (!peer.isEmpty()) {
                openConversation(peer);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace selecta::app
