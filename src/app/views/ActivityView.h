// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QTabWidget;
class QTimer;
class QTreeView;

namespace selecta::core {
class EngineHost;
}
namespace selecta::storage {
class Database;
}

namespace selecta::app {

class ActivityModel;
class PeersModel;
class TransfersModel;

// What Selecta does on the network, in three pages: the transfers (PRD DL-1), by folder or by peer, with what each
// file is doing and Cancel or Try Again; the peers files came from, against the daily limits (FAIR-1); and the log
// of every search, request and refusal with the reason (SAFE-4), with the search budget. A banner says when network
// activity is paused. Main thread; reads the log through the main thread's database connection, then follows the
// engine.
class ActivityView : public QWidget
{
    Q_OBJECT

public:
    enum class Page { Transfers, Peers, Log };

    ActivityView(core::EngineHost &engine, storage::Database &db, QWidget *parent = nullptr);

    ActivityModel *model() const { return m_model; }
    TransfersModel *transfers() const { return m_transfers; }
    PeersModel *peers() const { return m_peers; }
    void showPage(Page page);
    // One line about transfers or the network's state, for the window subtitle.
    QString summary() const;

Q_SIGNALS:
    void summaryChanged();

private:
    QWidget *createTransfersPage();
    QWidget *createPeersPage();
    QWidget *createLogPage();
    void reload();
    void refreshState();
    void refreshPeers();
    void transfersMenu(QPoint position);

    core::EngineHost &m_engine;
    storage::Database &m_db;
    ActivityModel *m_model;
    TransfersModel *m_transfers;
    PeersModel *m_peers;
    QWidget *m_pausedBanner;
    QTabWidget *m_pages;
    QLabel *m_budget;
    QTreeView *m_table;
    QTreeView *m_transfersTable;
    QComboBox *m_grouping;
    QTreeView *m_peersTable;
    QLabel *m_peersNote;
    QTimer *m_flushTimer; // paces progress on screen
};

} // namespace selecta::app
