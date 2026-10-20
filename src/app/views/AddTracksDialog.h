// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QPlainTextEdit;
class QPushButton;

namespace selecta::app {

// "Add tracks" (PRD BATCH-1): a sheet to paste a tracklist, one track per line, or Spotify links; a text file can be
// opened into it too. Returns the text; parsing happens in the engine.
class AddTracksDialog : public QDialog
{
    Q_OBJECT

public:
    // Text files larger than this are refused: a tracklist is a few kilobytes.
    static constexpr qint64 kMaxFileBytes = qint64{1024} * 1024;

    explicit AddTracksDialog(QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);

private:
    void openFile();
    void refreshButtons();

    QPlainTextEdit *m_editor;
    QPushButton *m_add = nullptr;
};

} // namespace selecta::app
