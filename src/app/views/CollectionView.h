// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Albums.h"
#include "core/CollectionService.h"

#include <QWidget>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// The collection (PRD LIB-1, LIB-2): which folders Selecta indexes, how many tracks it knows, the scan in progress,
// and the files it couldn't read. The index tells Get tracks and searches what you already have; Selecta never changes
// these files. Main thread.
class CollectionView : public QWidget
{
    Q_OBJECT

public:
    explicit CollectionView(core::EngineHost &engine, QWidget *parent = nullptr);

    // One line for the window subtitle.
    QString summary() const;

Q_SIGNALS:
    void summaryChanged();
    void settingsRequested(); // "Choose folders…": the folders are chosen in Settings → Library

private:
    void show(const core::CollectionStatus &status);
    // DL-8: filed singles that are also on an album.
    void showFlags(const QList<core::SupersedeFlag> &flags);

    core::EngineHost &m_engine;
    core::CollectionStatus m_status;
    QStackedWidget *m_pages;
    QLabel *m_headline;
    QLabel *m_details;
    QLabel *m_message;
    QProgressBar *m_progress;
    QPushButton *m_scan;
    QLabel *m_unreadableTitle;
    QListWidget *m_unreadable;
    QLabel *m_flagsTitle;
    QListWidget *m_flags;
};

} // namespace selecta::app
