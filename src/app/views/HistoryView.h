// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QStandardItemModel;
class QTreeView;

namespace selecta::storage {
class Database;
}

namespace selecta::app {

// Sessions (launch to quit, PRD D8) with what was downloaded in each, from whom, in what quality, and where each file
// is now (PRD DL-9). Reads the database through the main thread's connection.
class HistoryView : public QWidget
{
    Q_OBJECT

public:
    HistoryView(storage::Database &db, int currentSession, QWidget *parent = nullptr);

    // Reloads sessions and downloads from the database.
    void refresh();

protected:
    void showEvent(QShowEvent *event) override;

private:
    storage::Database &m_db;
    int m_currentSession;
    QStandardItemModel *m_model;
    QTreeView *m_table;
    QLabel *m_summary;
};

} // namespace selecta::app
