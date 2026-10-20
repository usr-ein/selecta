// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class GetTracksInspector;
class GetTracksModel;

// Get tracks, the home screen (PRD D27, BATCH-9): paste a list, each line is matched to its release, found on
// Soulseek, previewed and got. The engine does the work; this view shows the list and passes the user's choices on.
// Main thread.
class GetTracksView : public QWidget
{
    Q_OBJECT

public:
    explicit GetTracksView(core::EngineHost &engine, QWidget *parent = nullptr);

    GetTracksModel *model() const { return m_model; }
    // The inspector panel that follows this view's selection; the window hosts it.
    GetTracksInspector *inspectorPanel() const { return m_inspector; }
    // One line for the window subtitle.
    QString summary() const;
    QList<qint64> selectedIds() const;

public Q_SLOTS:
    // Opens the "Add tracks" sheet.
    void addTracks();
    void removeSelected();

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void summaryChanged();

private:
    void refresh();
    void showSelection();
    void showAdded(int added, int duplicates, int unidentified, const QString &note);

    core::EngineHost &m_engine;
    GetTracksModel *m_model;
    GetTracksInspector *m_inspector;
    QStackedWidget *m_pages;
    QTreeView *m_table;
    QLabel *m_summary;
    QLabel *m_note;
    QPushButton *m_add;
};

} // namespace selecta::app
