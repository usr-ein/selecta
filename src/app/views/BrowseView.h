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

class SearchResultsModel;

// A peer's shared folders (PRD BRWS-1): its files by folder, with the quality, length and what the user already has,
// under the same filters as searches. Files download like search results; a folder downloads whole, as an album. One
// view shows whichever browse is picked in the sidebar. Main thread only.
class BrowseView : public QWidget
{
    Q_OBJECT

public:
    BrowseView(core::EngineHost &engine, AppSettings &settings, QWidget *parent = nullptr);

    // Shows this peer's shares (as browsed so far), or nothing for an empty name.
    void showPeer(const QString &peer);
    QString peer() const { return m_peer; }
    QString title() const;
    QString summary() const;
    SearchResultsModel *model() const { return m_model; }
    // The selected files (a folder row stands for its files), and the file standing for the current row's folder.
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
    void downloadSelected();
    void downloadFolder();
    void contextMenu(QPoint position);

    core::EngineHost &m_engine;
    AppSettings &m_settings;
    QString m_peer;
    SearchResultsModel *m_model;
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
