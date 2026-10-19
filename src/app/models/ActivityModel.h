// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ActivityLog.h"

#include <QAbstractTableModel>
#include <QList>

namespace selecta::app {

// The network activity log as a table, newest line first (PRD SAFE-4). Filled once from the database, then kept current
// from the engine's live lines; a line already shown is ignored, so the two sources may overlap. Holds at most
// kMaxRows lines: the database keeps the rest. Main thread only.
class ActivityModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        Time,
        Operation,
        Subject,
        Outcome,
        ColumnCount
    };
    static constexpr int kMaxRows = 2000;
    static constexpr int kEntryRole = Qt::UserRole + 1; // the row's ActivityEntry, as a QVariant

    explicit ActivityModel(QObject *parent = nullptr);

    // Replaces every row with these lines, newest first.
    void reset(const QList<core::ActivityEntry> &newestFirst);
    // Adds a live line at the top, unless a line with this id is already shown.
    void prepend(const core::ActivityEntry &entry);
    core::ActivityEntry entryAt(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // "queue-download" → "Queue download"; "rate-limited" → "Rate limited".
    static QString humanise(const QString &name);
    // Whether this outcome is a refusal or a failure (shown as a warning).
    static bool isWarning(const QString &outcome);

private:
    QList<core::ActivityEntry> m_entries; // newest first
    qint64 m_newestId = 0;
};

} // namespace selecta::app
