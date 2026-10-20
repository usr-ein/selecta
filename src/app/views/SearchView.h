// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/AppSettings.h"
#include "core/ManualSearchService.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimeEdit;
class QTimer;
class QToolButton;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class SearchInspector;
class SearchResultsModel;

// One search typed by hand (PRD SRCH-1…19, UI-7): the quality wanted, the release the results are ranked against,
// "N of M match" with why the others are hidden, and the results (flat or by folder), each previewed and downloaded
// from here. The filters behind the Filters button apply to every search and persist. The engine searches; this view
// shows what came back. Main thread only.
class SearchView : public QWidget
{
    Q_OBJECT

public:
    SearchView(core::EngineHost &engine, AppSettings &settings, QWidget *parent = nullptr);

    // Shows this search (its results so far), or the empty page for 0.
    void showSearch(qint64 searchId);
    qint64 searchId() const { return m_searchId; }
    SearchInspector *inspectorPanel() const { return m_inspector; }
    SearchResultsModel *model() const { return m_model; }
    // The search text, and one line for the window subtitle.
    QString title() const;
    QString summary() const;

    // What Space or a double-click previews: the current result.
    struct PreviewTarget
    {
        qint64 searchId = 0;
        int index = -1;
        QString peer;
    };
    std::optional<PreviewTarget> previewTarget() const;
    // The selected results (a folder row stands for its files).
    QList<int> selectedResults() const;
    // Downloads the selected results; asks first about the ones the user already has (DL-11). Several tracks of one
    // folder: offers the whole folder as an album instead (DL-12).
    void downloadSelected();
    // Downloads the folder of the current result (or folder row) whole, as one Inbox album, once confirmed (DL-1).
    void downloadFolder();
    // The result that stands for the current row's folder: the current result, or a folder row's first track.
    std::optional<int> folderAnchor() const;

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    // The current row moved (↑, ↓, a click): previews may follow it.
    void selectionMoved();
    void summaryChanged();
    void previewRequested();
    // "Browse <peer>'s Files" (BRWS-1).
    void browseRequested(const QString &peer);
    // "Message <peer>…" (NET-7).
    void messageRequested(const QString &peer);

private:
    QWidget *createFilterBar();
    QWidget *createFiltersPanel();
    void applyFilter(const core::SearchFilter &filter, bool customised);
    void filtersEdited();
    void loadFilterControls();
    void refreshHeader();
    void refreshWishlistButton();
    void refreshSummary();
    void refreshInspector();
    void keepSelection(const std::function<void()> &change);
    // The rest of downloadSelected(): the duplicate guard, then the download.
    void downloadFiles(const QList<int> &selected);
    // The audio results of `anchor`'s folder (same peer, same folder) the search found.
    QList<const core::SearchResult *> folderFiles(const core::SearchResult &anchor) const;

    core::EngineHost &m_engine;
    AppSettings &m_settings;
    AppSettings::SearchViewState m_state;
    SearchResultsModel *m_model;
    SearchInspector *m_inspector;
    qint64 m_searchId = 0;
    bool m_loadingControls = false;

    QStackedWidget *m_pages;
    QComboBox *m_preset;
    QCheckBox *m_onlyRelease;
    QCheckBox *m_grouped;
    QToolButton *m_filtersButton;
    QPushButton *m_download;
    QPushButton *m_watch;               // Add to Wishlist (SRCH-14)
    QList<QCheckBox *> m_formatButtons; // by core::SearchFormat
    QComboBox *m_bitrateRule;
    QComboBox *m_bitrate;
    QCheckBox *m_cbrOnly;
    QComboBox *m_bitDepth;
    QList<QCheckBox *> m_rateButtons; // by kSampleRates
    QTimeEdit *m_minLength;
    QTimeEdit *m_maxLength;
    QCheckBox *m_freeSlot;
    QComboBox *m_maxQueue;
    QCheckBox *m_strict;
    QComboBox *m_owned;
    QLineEdit *m_excluded;
    QWidget *m_filtersPanel; // built from the controls above
    QWidget *m_releaseRow;
    QComboBox *m_release;
    QLabel *m_releaseNote;
    QLabel *m_summary;
    QLabel *m_status;
    QTreeView *m_table;
    QTimer *m_refreshTimer; // paces the table while results stream in
    QTimer *m_clockTimer;   // keeps "cached 3 min ago" and "searching…" current
};

} // namespace selecta::app
