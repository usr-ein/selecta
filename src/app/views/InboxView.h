// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/InboxService.h"
#include "playback/Player.h"

#include <QList>
#include <QWidget>

#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class InboxInspector;

// The Inbox (PRD DL-4): downloads waiting to be filed, albums and singles, with their format, file name and source.
// File moves the selection into the collection; Move to Trash sends it to the macOS Trash. The engine does the work;
// this view shows its list and its results. Main thread.
class InboxView : public QWidget
{
    Q_OBJECT

public:
    explicit InboxView(core::EngineHost &engine, QWidget *parent = nullptr);

    void setLibraryConfig(const core::LibraryConfig &config);
    // One line for the window subtitle.
    QString summary() const;
    QList<qint64> selectedIds() const;
    // The inspector panel that follows this view's selection; the window hosts it.
    InboxInspector *inspectorPanel() const { return m_inspector; }

    // What Space or a double-click previews: the current row's file, or an album folder (its first track plays).
    struct PreviewTarget
    {
        QString path;
        playback::PlayerTrack track;
    };
    std::optional<PreviewTarget> previewTarget() const;

public Q_SLOTS:
    void fileSelected();
    void trashSelected();
    void revealSelected();

Q_SIGNALS:
    // The current row moved (↑, ↓, a click): previews may follow it.
    void selectionMoved();
    void summaryChanged();
    void previewRequested(); // a double-click on an item

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuild(const QList<core::InboxEntry> &entries);
    void refreshActions();
    void showSelection();
    void showResult(qint64 id, bool ok, const QString &message);

    core::EngineHost &m_engine;
    core::LibraryConfig m_config;
    QList<core::InboxEntry> m_entries;
    QStandardItemModel *m_model;
    QStackedWidget *m_pages;
    QTreeView *m_table;
    QLabel *m_summary;
    QLabel *m_result;
    QPushButton *m_file;
    QPushButton *m_trash;
    InboxInspector *m_inspector;
    std::optional<library::InboxItem> m_shownItem; // what the inspector shows
};

} // namespace selecta::app
