// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Downloads.h"
#include "core/ShareService.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

namespace selecta::app {

// The transfer list (PRD DL-1): this session's downloads and the unfinished ones, grouped by remote folder (an album
// downloads as one folder) or by peer, then the uploads. Each row says where the file is: waiting, queued at the peer
// (with its place), connecting, arriving (percent, speed, time left), converting, in the Inbox, failed (with why) or
// cancelled. Speeds come from the progress reports, smoothed. Main thread only.
class TransfersModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        ItemColumn,
        PeerColumn,
        StatusColumn,
        ProgressColumn,
        ColumnCount
    };
    enum class Grouping { Folder, Peer };
    static constexpr int DownloadIdRole = Qt::UserRole + 1; // qint64: the download; -1 for a group or an upload
    static constexpr int IsGroupRole = Qt::UserRole + 2;    // bool

    // What the window subtitle says.
    struct Counts
    {
        int downloading = 0; // arriving now
        int waiting = 0;     // waiting, queued or connecting
        int uploading = 0;
    };

    explicit TransfersModel(QObject *parent = nullptr);

    void setGrouping(Grouping grouping);
    Grouping grouping() const { return m_grouping; }
    // Every download again, with the bytes each has on disk and its size.
    void reset(const QList<core::Download> &downloads, const QHash<qint64, std::pair<qint64, qint64>> &progress);
    void update(const core::Download &download);
    // Bytes on disk and the file's size at `nowMs` (monotonic, ms): the row's speed follows. Shown at flush().
    void progress(qint64 id, qint64 bytesDone, qint64 bytesTotal, qint64 nowMs);
    void setUploads(const QList<core::UploadEntry> &uploads);
    // Shows the progress reported since the last flush (the view paces it).
    void flush();

    Counts counts() const;
    std::optional<core::Download> downloadAt(const QModelIndex &index) const;
    // The downloads a row stands for: its own, or a group's.
    QList<qint64> downloadsAt(const QModelIndex &index) const;
    // "42 % · 1.3 MB/s · 0:12 left", "Queued at vinyl_ghost, place 3", "Failed: File not shared."
    QString statusText(qint64 id) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    struct Row
    {
        core::Download download;
        qint64 done = 0;    // bytes on disk
        qint64 total = 0;   // the file's size
        double speed = 0.0; // bytes per second, smoothed
        qint64 sampleBytes = -1;
        qint64 sampleMs = 0;
    };
    struct Group
    {
        QString key;
        QString title;
        QString peer;          // the folder's peer (Folder grouping)
        QList<qint64> members; // download ids, oldest first
        bool uploads = false;  // the uploads' group, last
    };

    struct GroupSummary
    {
        int arrived = 0;
        int failed = 0;
        int arriving = 0;
        qint64 done = 0;
        qint64 total = 0;
    };

    QString groupKey(const core::Download &download) const;
    GroupSummary summarise(const Group &group) const;
    void rebuild();
    void place(qint64 id, bool announce);
    int groupOf(qint64 id) const;
    QVariant downloadData(const Row &row, int column, int role) const;
    QVariant groupData(const Group &group, int column, int role) const;
    QVariant uploadData(const core::UploadEntry &upload, int column, int role) const;
    double fraction(const Row &row) const;

    Grouping m_grouping = Grouping::Folder;
    QHash<qint64, Row> m_rows;
    QList<Group> m_groups; // downloads' groups in order of their first download, then the uploads
    QList<core::UploadEntry> m_uploads;
    QSet<qint64> m_dirty; // rows whose progress moved since the last flush
};

} // namespace selecta::app
