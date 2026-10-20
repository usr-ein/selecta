// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"

#include <QWidget>

#include <optional>

class QFormLayout;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace selecta::app {

// The inspector for Get tracks (PRD BATCH-9): the selected line's release, which is the tags the file will get, and
// the file chosen on Soulseek. Main thread.
class GetTracksInspector : public QWidget
{
    Q_OBJECT

public:
    explicit GetTracksInspector(QWidget *parent = nullptr);

    // Shows this line, or the empty state for nothing.
    void showLine(const std::optional<core::GetLine> &line);
    std::optional<qint64> lineId() const;

private:
    void addRow(QFormLayout *form, const QString &label, QLabel *&value, const QString &objectName);

    QStackedWidget *m_pages;
    QLabel *m_title;
    QLabel *m_status;
    QLabel *m_releaseHeading = nullptr;
    QWidget *m_releaseBox;
    QLabel *m_releaseTitle = nullptr;
    QLabel *m_releaseArtist = nullptr;
    QLabel *m_releaseAlbum = nullptr;
    QLabel *m_releaseDate = nullptr;
    QLabel *m_releaseLength = nullptr;
    QLabel *m_releaseTrack = nullptr;
    QLabel *m_releaseIsrc = nullptr;
    QLabel *m_releaseLabel = nullptr;
    QPushButton *m_openRelease;
    QLabel *m_noRelease;
    QLabel *m_fileHeading = nullptr;
    QLabel *m_file;
    std::optional<core::GetLine> m_line;
};

} // namespace selecta::app
