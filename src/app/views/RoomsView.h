// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/RoomService.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItemModel;
class QStringListModel;
class QTextBrowser;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// Minimal chat rooms (PRD NET-10, D32). On the left, the rooms you're in with what's unread, and the server's room
// list to join from, filtered as you type (a name nobody uses creates a room, after asking). On the right, the open
// room's lines as they arrive, its members, and a field to post. The server keeps no history: a room shows what was
// said since you joined. Main thread only.
class RoomsView : public QWidget
{
    Q_OBJECT

public:
    explicit RoomsView(core::EngineHost &engine, QWidget *parent = nullptr);

    // Shows a room you're in (or joining).
    void openRoom(const QString &room);
    QString room() const { return m_room; }
    QString summary() const;
    // Lines unread in every room, for the sidebar.
    int unread() const;
    // Whether the user is reading this room now (the view shown, the window active).
    bool isReading(const QString &room) const;

Q_SIGNALS:
    void summaryChanged();
    void unreadChanged();
    void browseRequested(const QString &peer);
    void messageRequested(const QString &peer);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QWidget *createLeft();
    QWidget *createRoomPage();
    void rebuildJoined();
    void rebuildList(const QList<core::RoomListing> &rooms);
    void refreshRoom();
    void renderLines();
    void appendLine(const core::RoomLine &line);
    void joinTyped();
    void joinSelected();
    // Joins a room by name, asking first when it isn't in the list (joining creates it).
    void join(const QString &room);
    void post();
    void membersMenu(QPoint position);
    // Tells the engine which room is on screen, if any.
    void updateViewing();

    core::EngineHost &m_engine;
    QString m_room;    // the open room
    QString m_viewing; // what the engine was told is on screen
    QString m_opening; // a room the user just joined: opened as soon as the engine has it
    QMap<QString, core::Room> m_rooms;
    QList<core::RoomListing> m_listings;
    QStandardItemModel *m_joinedModel;
    QTreeView *m_joined;
    QLineEdit *m_filter;
    QStandardItemModel *m_listModel;
    QSortFilterProxyModel *m_listProxy;
    QTreeView *m_list;
    QPushButton *m_join;
    QLabel *m_joinProblem;
    QStackedWidget *m_pages;
    QLabel *m_name;
    QLabel *m_details;
    QPushButton *m_leave;
    QTextBrowser *m_lines;
    QStringListModel *m_membersModel;
    QListView *m_members;
    QLabel *m_problem;
    QLineEdit *m_input;
    QPushButton *m_send;
};

} // namespace selecta::app
