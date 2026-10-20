// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include <optional>

class QElapsedTimer;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QStackedWidget;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class Presets;

class GetTracksInspector;
class GetTracksModel;

// Get tracks, the home screen (PRD D27, BATCH-9): paste a list, each line is matched to its release, found on
// Soulseek, previewed and got. The engine does the work; this view shows the list and passes the user's choices on.
// Main thread.
class GetTracksView : public QWidget
{
    Q_OBJECT

public:
    GetTracksView(core::EngineHost &engine, Presets &presets, QWidget *parent = nullptr);

    GetTracksModel *model() const { return m_model; }
    // The inspector panel that follows this view's selection; the window hosts it.
    GetTracksInspector *inspectorPanel() const { return m_inspector; }
    // One line for the window subtitle.
    QString summary() const;
    QList<qint64> selectedIds() const;

    // What Space or a double-click previews: the current line's chosen file.
    struct PreviewTarget
    {
        qint64 lineId = 0;
        QString identity; // as the engine names the preview (peer, path, size); empty without a file
        QString peer;
    };
    std::optional<PreviewTarget> previewTarget() const;

public Q_SLOTS:
    // Opens the "Add tracks" sheet.
    void addTracks();
    void removeSelected();

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    // The current row moved (↑, ↓, a click): previews may follow it.
    void selectionMoved();
    void summaryChanged();
    void previewRequested(); // a double-click on a line

private:
    void refresh();
    void showSelection();
    void showAdded(int added, int duplicates, int unidentified, const QString &note);
    void refreshProgress();
    // The preset menu follows the saved presets (SRCH-8).
    void rebuildPresets();
    void contextMenu(QPoint position);
    // BATCH-8: saves the lines nothing was found for (or no sure release), one per line, as pasted.
    void exportUnresolved();
    QStringList unresolvedLines() const;

    core::EngineHost &m_engine;
    Presets &m_presets;
    GetTracksModel *m_model;
    GetTracksInspector *m_inspector;
    QStackedWidget *m_pages;
    QTreeView *m_table;
    QLabel *m_summary;
    QLabel *m_note;
    QPushButton *m_add;
    QComboBox *m_preset; // the quality wanted (BATCH-2, SRCH-8)
    QPushButton *m_find;
    QPushButton *m_get;
    QPushButton *m_export; // BATCH-8: the lines not found, as text
    QLabel *m_progress;
    QTimer *m_countdown;
    int m_waiting = 0;     // searches waiting for the governor
    int m_collecting = 0;  // searches collecting results
    qint64 m_nextAtMs = 0; // when the governor allows the next search (steady clock, ms)
};

} // namespace selecta::app
