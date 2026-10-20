// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QList>

namespace selecta::app {

// The Get tracks lines as a table (PRD BATCH-9): the tick, the track with its matched release, the status, and the
// best file on Soulseek. Mirrors the engine's list: reset() when lines come or go, update() when one changes. Ticking
// a line asks the engine through tickRequested(); the tick shows once the engine confirms. Main thread only.
class GetTracksModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        TickColumn,
        TrackColumn,
        StatusColumn,
        FileColumn,
        ColumnCount
    };
    enum Role { // NOLINT(cppcoreguidelines-use-enum-class): Qt item roles are plain ints
        LineIdRole = Qt::UserRole + 1,
        SubtitleRole,   // the second line under the track: its release, or why there is none
        StatusKindRole, // a StatusKind, for colouring
    };
    enum class StatusKind { Waiting, Working, Good, Check, Bad, Have };

    explicit GetTracksModel(QObject *parent = nullptr);

    void reset(const QList<core::GetLine> &lines);
    void update(const core::GetLine &line);
    const core::GetLine *lineAt(int row) const;
    int rowOf(qint64 id) const;
    const QList<core::GetLine> &lines() const { return m_lines; }

    // How many lines have each outcome, for the summary line.
    struct Counts
    {
        int total = 0;
        int lookingUp = 0; // a release lookup is running
        int waiting = 0;   // waiting for a release lookup
        int released = 0;  // a release was found
        int ready = 0;
        int check = 0;
        int notFound = 0;
        int have = 0; // in the collection or the Inbox
        int ticked = 0;
    };
    Counts counts() const;

    static QString statusText(const core::GetLine &line);
    static StatusKind statusKind(const core::GetLine &line);
    static QString subtitle(const core::GetLine &line);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

Q_SIGNALS:
    void tickRequested(qint64 id, bool ticked);

private:
    QList<core::GetLine> m_lines;
    QHash<qint64, int> m_rows; // line id → row
};

} // namespace selecta::app
