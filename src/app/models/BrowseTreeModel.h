// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchFilter.h"
#include "core/SearchResults.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace selecta::app {

// A peer's whole share list as a tree (PRD BRWS-1): its folders nested as the peer has them, with every file in them.
// The search filters don't hide anything here: a file they would hide is dimmed, and its tooltip says why. Folders come
// first, then files, each in natural order ("2" before "10") or by a column.
//
// The results are a browse's (core::BrowseService), analysed like search results; a path is split on "\" and "/", and
// its empty parts are skipped, so any path the peer sends lands somewhere in the tree. Rebuilt on reset() and sort();
// setFilter() only restyles the rows. Main thread only.
class BrowseTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        NameColumn,
        StatusColumn,
        QualityColumn,
        LengthColumn,
        SizeColumn,
        ColumnCount
    };
    enum Role {                             // NOLINT(cppcoreguidelines-use-enum-class): Qt item roles are plain ints
        ResultIndexRole = Qt::UserRole + 1, // int: the result's index, -1 for a folder row
        IsFolderRole,                       // bool
        HiddenReasonRole,                   // int (core::HiddenReason): why the search filters would hide the file
    };

    explicit BrowseTreeModel(QObject *parent = nullptr);

    void reset(const QList<core::SearchResult> &results);
    void setFilter(const core::SearchFilter &filter);
    core::SearchFilter filter() const { return m_filter; }

    // Every file, and how many the search filters would show (SRCH-9).
    core::FilterCounts counts() const { return m_counts; }
    int fileCount() const { return static_cast<int>(m_results.size()); }
    int folderCount() const { return m_folderCount; }

    // The result behind a row; nullptr for a folder row or an invalid index.
    const core::SearchResult *resultAt(const QModelIndex &index) const;
    const core::SearchResult *result(int resultIndex) const;
    QModelIndex indexOfResult(int resultIndex, int column = 0) const;
    // The row of the file at this path, as the peer sent it; invalid when the list doesn't have it.
    QModelIndex indexOfPath(const QByteArray &rawPath, int column = 0) const;
    // The files directly in a folder row (not in its subfolders), in display order.
    QList<int> filesIn(const QModelIndex &folder) const;
    // Whether the search filters would hide this result.
    core::HiddenReason hiddenReason(int resultIndex) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    struct Node
    {
        QString name;
        int parent = -1; // node id; -1 for the root
        int row = 0;     // among the parent's children
        int result = -1; // files: the result's index
        QList<int> children;
        qint64 size = 0; // folders: every file under them
        int files = 0;   // folders: every file under them
    };

    void rebuild();
    void restyle();
    void sortChildren(int node);
    bool lessThan(int a, int b) const;
    QModelIndex indexOfNode(int node, int column) const;
    // A folder's whole path, from its parents (not kept per folder: that would cost depth² on a deep tree).
    QString pathOf(int node) const;
    QVariant fileData(const core::SearchResult &result, int column, int role) const;
    QVariant folderData(int node, int column, int role) const;

    QList<core::SearchResult> m_results; // by index
    QList<core::HiddenReason> m_hidden;  // by result index
    core::SearchFilter m_filter;
    core::FilterCounts m_counts;
    QList<Node> m_nodes;                   // 0 is the invisible root
    QList<int> m_nodeOfResult;             // by result index
    QHash<QByteArray, int> m_resultOfPath; // raw path → result index
    int m_folderCount = 0;
    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};

} // namespace selecta::app
