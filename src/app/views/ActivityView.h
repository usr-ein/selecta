// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeView;
class QWidget;

namespace selecta::core {
class EngineHost;
}
namespace selecta::storage {
class Database;
}

namespace selecta::app {

class ActivityModel;

// What Selecta did on the network (PRD SAFE-4): every search, request and refusal, with the reason, plus the search
// budget and the pause switch's state. Transfers and per-peer counts join it with the transfer service. Main thread;
// reads the log through the main thread's database connection, then follows the engine.
class ActivityView : public QWidget
{
    Q_OBJECT

public:
    ActivityView(core::EngineHost &engine, storage::Database &db, QWidget *parent = nullptr);

    ActivityModel *model() const { return m_model; }
    // One line about the network's state, for the window subtitle.
    QString summary() const;

Q_SIGNALS:
    void summaryChanged();

private:
    void reload();
    void refreshState();

    core::EngineHost &m_engine;
    storage::Database &m_db;
    ActivityModel *m_model;
    QWidget *m_pausedBanner;
    QLabel *m_budget;
    QTreeView *m_table;
};

} // namespace selecta::app
