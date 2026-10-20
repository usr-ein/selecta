// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/ReleaseResolver.h"

#include <QDialog>
#include <QList>

class QDialogButtonBox;
class QLabel;
class QListWidget;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// Picks another release for an Inbox single (PRD DL-4, META-4): the engine looks the track up (Spotify, Deezer), the
// candidates are listed with their confidence, and the chosen one re-tags the file (tags, label and genre, cover).
// Shown as a sheet. Main thread only.
class ReleasePickerDialog : public QDialog
{
    Q_OBJECT

public:
    ReleasePickerDialog(core::EngineHost &engine, qint64 itemId, const QString &track, QWidget *parent = nullptr);

private:
    void showCandidates(qint64 itemId, const QList<meta::Candidate> &candidates, const QString &error);

    core::EngineHost &m_engine;
    qint64 m_itemId;
    QList<meta::Candidate> m_candidates;
    QLabel *m_status;
    QListWidget *m_list;
    QDialogButtonBox *m_buttons;
};

} // namespace selecta::app
