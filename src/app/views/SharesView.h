// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ShareService.h"

#include <QWidget>

class QLabel;
class QTreeWidget;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// Shares (PRD SHARE-1…5, FAIR-3): what Selecta shares (the collection, read-only), what peers are getting from it now,
// and the ratio of what was given back to what was taken, with what to do about it. Main thread only.
class SharesView : public QWidget
{
    Q_OBJECT

public:
    explicit SharesView(core::EngineHost &engine, QWidget *parent = nullptr);

    QString summary() const;

Q_SIGNALS:
    void summaryChanged();
    void settingsRequested(); // "Choose folders…": Settings → Library

private:
    void showStatus(const core::ShareStatus &status);
    void showUploads(const QList<core::UploadEntry> &uploads);

    core::EngineHost &m_engine;
    core::ShareStatus m_status;
    QLabel *m_headline;
    QLabel *m_folders;
    QLabel *m_message;
    QLabel *m_activity;
    QLabel *m_ratio;
    QLabel *m_guidance;
    QLabel *m_noUploads;
    QTreeWidget *m_uploads;
};

} // namespace selecta::app
