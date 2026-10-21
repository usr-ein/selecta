// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/AppSettings.h"
#include "core/BrowseService.h"

#include <QWidget>

#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class BrowseTreeModel;

// A peer's shared folders (PRD BRWS-1): their whole tree, every file with its quality, length and what the user already
// has. Files the search filters would hide are dimmed, not hidden; or the view says why there's no list (offline, no
// answer, refused, too big). Files download like search results; a folder downloads whole, as an album. One view shows
// whichever browse is picked in the sidebar. Main thread only.
class BrowseView : public QWidget
{
    Q_OBJECT

public:
    BrowseView(core::EngineHost &engine, AppSettings &settings, QWidget *parent = nullptr);

    // Shows this peer's shares (as browsed so far), or nothing for an empty name.
    void showPeer(const QString &peer);
    // The next time this peer's shares show, the file at `remotePath` (as the peer sent it) is selected, its folders
    // open, as soon as the list has it; if the list comes without it, the view says so. An empty path forgets one asked
    // for before.
    void revealFile(const QString &peer, const QByteArray &remotePath);
    QString peer() const { return m_peer; }
    QString title() const;
    QString summary() const;
    BrowseTreeModel *model() const { return m_model; }
    // The selected files: audio files picked one by one, and for a folder row the audio files directly in it that
    // pass the search filters. And the file standing for the current row's folder.
    QList<int> selectedResults() const;
    std::optional<int> folderAnchor() const;
    // What Space previews: the current file.
    std::optional<int> previewTarget() const;

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void summaryChanged();
    void previewRequested();
    void selectionMoved();
    // "Message <peer>…" (NET-7).
    void messageRequested(const QString &peer);

private:
    void refresh();
    void openFolders();
    void updateButtons();
    // The current file's path, as the peer sent it; empty on a folder or nothing.
    QByteArray currentPath() const;
    // Opens the file's folders, selects it and scrolls to it.
    void select(const QModelIndex &file);
    // Selects the file revealFile() asked for, if the list has it. `final`: this list is the answer, so a file it
    // lacks is reported rather than waited for.
    void revealPending(bool final);
    void downloadSelected();
    void downloadFolder();
    void contextMenu(QPoint position);

    core::EngineHost &m_engine;
    AppSettings &m_settings;
    QString m_peer;
    QString m_revealPeer;
    QByteArray m_revealPath;
    QString m_missing; // the file revealFile() asked for that the list didn't have, by name
    BrowseTreeModel *m_model;
    QLabel *m_headline;
    QLabel *m_details;
    QPushButton *m_refresh;
    QPushButton *m_download;
    QPushButton *m_downloadFolder;
    QStackedWidget *m_pages;
    QLabel *m_waiting;
    QTreeView *m_table;
};

} // namespace selecta::app
