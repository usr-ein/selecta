// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SocialService.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;
class QTabWidget;
class QTextBrowser;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// Private messages and the user lists (PRD NET-7, NET-8), in two pages. Conversations: the list, the latest first
// with what is unread, and the open one's messages with a field to reply; a username typed above the list starts a
// new one. People: buddies (online or not), ignored users, whose messages are dropped, and banned users, who get
// nothing of our shares. A conversation shown while the window is active counts as read. Main thread only.
class MessagesView : public QWidget
{
    Q_OBJECT

public:
    enum class Page { Conversations, People };

    explicit MessagesView(core::EngineHost &engine, QWidget *parent = nullptr);

    // Shows the conversation with this user, a new one when there is none.
    void openConversation(const QString &peer);
    void showPage(Page page);
    // The open conversation's user, or an empty string.
    QString peer() const { return m_peer; }
    // Whether the user is reading this conversation now (the view shown, the window active).
    bool isReading(const QString &peer) const;
    QString summary() const;
    // Unread messages in every conversation, for the sidebar and the Dock icon.
    int unread() const;

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void summaryChanged();
    void browseRequested(const QString &peer);

protected:
    void showEvent(QShowEvent *event) override;

private:
    QWidget *createConversationsPage();
    QWidget *createThread();
    QWidget *createPeoplePage();
    void rebuildConversations();
    void rebuildPeople();
    std::optional<core::ListedUser> listedUser(const QString &peer) const;
    void renderThread();
    void refreshThreadBar();
    void send();
    void sent(const QString &peer, const QString &problem);
    void markOpenRead();
    void startTyped();
    void addPerson();
    void peopleMenu(QPoint position);
    void conversationsMenu(QPoint position);
    QStringList selectedPeople() const;

    core::EngineHost &m_engine;
    QString m_peer; // the open conversation
    QList<core::ChatMessage> m_messages;
    QList<core::Conversation> m_conversationList;
    QList<core::ListedUser> m_users;
    bool m_sending = false;
    QTabWidget *m_pages;
    // Conversations
    QLineEdit *m_to;
    QStandardItemModel *m_conversations;
    QTreeView *m_list;
    QStackedWidget *m_threadPages;
    QLabel *m_peerName;
    QLabel *m_peerState;
    QPushButton *m_browse;
    QPushButton *m_buddy;
    QPushButton *m_more;
    QTextBrowser *m_thread;
    QLabel *m_problem;
    QLineEdit *m_input;
    QPushButton *m_send;
    // People
    QLineEdit *m_person;
    QComboBox *m_listChoice;
    QStandardItemModel *m_people;
    QTreeView *m_peopleTable;
};

} // namespace selecta::app
