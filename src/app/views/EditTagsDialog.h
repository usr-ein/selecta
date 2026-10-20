// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

class QLineEdit;

namespace selecta::app {

// Edits the basic tags of an Inbox single (PRD DL-4): title, artists, album, album artist, date, track number and
// genre. Shown as a sheet; the engine writes the changes. Main thread only.
class EditTagsDialog : public QDialog
{
    Q_OBJECT

public:
    EditTagsDialog(const QString &fileName, const convert::MetadataModel &tags, QWidget *parent = nullptr);

    // The fields typed differently from the file's (marked Provenance::User), and the fields emptied.
    convert::MetadataModel changes() const;
    QStringList cleared() const;

private:
    QHash<QString, QLineEdit *> m_fields; // by tag key
    convert::MetadataModel m_original;
};

} // namespace selecta::app
