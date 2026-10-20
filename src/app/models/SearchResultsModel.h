// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchFilter.h"
#include "core/SearchResults.h"

#include <QAbstractItemModel>
#include <QList>
#include <QString>

namespace selecta::app {

// One search's results for the Search view (PRD SRCH-9…11, SRCH-18): filtered with a core::SearchFilter, sorted by
// best match or by a column, flat or grouped by remote folder. Mirrors the engine's list: reset() when every result
// changes, append() as they stream in (then refresh(), which the view paces), update() when some change.
//
// Every change rebuilds the visible rows with a model reset; the view keeps its selection by result index. Main thread
// only.
class SearchResultsModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column {    // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        TitleColumn, // first: it carries the folder disclosure when grouped, as the Finder's Name does
        StatusColumn,
        QualityColumn,
        LengthColumn,
        PeerColumn,
        AvailabilityColumn,
        ColumnCount
    };
    enum Role {                             // NOLINT(cppcoreguidelines-use-enum-class): Qt item roles are plain ints
        ResultIndexRole = Qt::UserRole + 1, // int: the result's index, -1 for a folder row
        IsFolderRole,                       // bool
    };
    static constexpr int kBestMatch = -1; // sort column for "Best match" (SRCH-18)

    explicit SearchResultsModel(QObject *parent = nullptr);

    void reset(const QList<core::SearchResult> &results);
    // New results, in index order. Shown at the next refresh().
    void append(const QList<core::SearchResult> &results);
    void update(const QList<core::SearchResult> &results);
    void refresh();
    bool isStale() const { return m_stale; }

    void setFilter(const core::SearchFilter &filter);
    core::SearchFilter filter() const { return m_filter; }
    // Whether the search ranks against a release, and its length (0 when unknown), for the length check.
    void setReleaseChosen(bool chosen, qint64 releaseDurationMs = 0);
    void setGrouped(bool grouped);
    bool isGrouped() const { return m_grouped; }
    int sortColumn() const { return m_sortColumn; }

    // Shown and hidden counts with the reasons (SRCH-9), over every result so far.
    core::FilterCounts counts() const { return m_counts; }
    int resultCount() const { return static_cast<int>(m_results.size()); }
    // The result behind a row; nullptr for a folder row or an invalid index.
    const core::SearchResult *resultAt(const QModelIndex &index) const;
    const core::SearchResult *result(int resultIndex) const;
    QModelIndex indexOfResult(int resultIndex, int column = 0) const;
    // The results shown under a folder row (grouped view), in display order.
    QList<int> resultsUnder(const QModelIndex &folder) const;

    static QString statusText(core::Ownership ownership);
    static QString qualityText(const core::SearchResult &result);
    static QString titleText(const core::SearchResult &result);

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    struct Folder
    {
        QString peer;
        QString path;
        QList<int> members; // result indexes, in display order
        qint64 size = 0;
    };

    void rebuild();
    bool lessThan(int a, int b) const;
    QVariant resultData(const core::SearchResult &result, int column, int role) const;
    // What VoiceOver reads for a cell: the display text with its symbols in words (UI-5).
    QString accessibleText(const core::SearchResult &result, int column) const;
    QVariant folderData(const Folder &folder, int column, int role) const;

    QList<core::SearchResult> m_results; // by index
    core::SearchFilter m_filter;
    bool m_releaseChosen = false;
    qint64 m_releaseDurationMs = 0;
    bool m_grouped = false;
    int m_sortColumn = kBestMatch;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_stale = false;
    core::FilterCounts m_counts;
    QList<int> m_shown;      // flat: result indexes in display order
    QList<Folder> m_folders; // grouped: folders in display order
};

} // namespace selecta::app
