// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QStandardItemModel;
class QTreeView;

namespace selecta::storage {
class SessionLog;
}

namespace selecta::app {

// Sessions (launch to quit, PRD D8) and, once downloads exist, what was downloaded in each. Reads the database on the
// main thread.
class HistoryView : public QWidget
{
    Q_OBJECT

public:
    HistoryView(storage::SessionLog &sessions, int currentSession, QWidget *parent = nullptr);

    // Reloads the session list from the database.
    void refresh();

protected:
    void showEvent(QShowEvent *event) override;

private:
    storage::SessionLog &m_sessions;
    int m_currentSession;
    QStandardItemModel *m_model;
    QTreeView *m_table;
    QLabel *m_summary;
};

} // namespace selecta::app
