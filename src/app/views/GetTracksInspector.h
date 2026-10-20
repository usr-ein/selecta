// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QWidget>

#include <optional>

class QButtonGroup;
class QFormLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace selecta::app {

// The inspector for Get tracks (PRD BATCH-9): the selected line's release, which is the tags the file will get, and
// the file chosen on Soulseek. When the line needs a way out (P-004), it also offers one:
//  - not found, with near misses: the track's files below the quality wanted, one to pick, and "Get Anyway";
//  - waiting in a peer's queue: the other peers' files, one to pick, and "Switch to This Peer";
//  - stuck in that queue: says so plainly, with one button, "Try Another Peer" (the picked file, else the best).
// The buttons only ask (the signals); the engine decides. Main thread.
class GetTracksInspector : public QWidget
{
    Q_OBJECT

public:
    explicit GetTracksInspector(QWidget *parent = nullptr);

    // Shows this line, or the empty state for nothing. The same line shown again keeps the file picked.
    void showLine(const std::optional<core::GetLine> &line);
    std::optional<qint64> lineId() const;

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    // "Get Anyway": the picked near miss.
    void getAnywayRequested(qint64 lineId, const QString &peer, const QByteArray &remotePath);
    // "Switch to This Peer" or "Try Another Peer": the picked file; an empty peer leaves the choice to the engine
    // (the best other peer, found again if none is known).
    void switchRequested(qint64 lineId, const QString &peer, const QByteArray &remotePath);
    // "Find Again": the line's search, again (after a restart, to see what it found).
    void findAgainRequested(qint64 lineId);

private:
    // A list of files to pick one from: a heading, a radio button per file, a note and a button.
    struct Choices
    {
        QWidget *box = nullptr;
        QLabel *heading = nullptr;
        QVBoxLayout *rows = nullptr;
        QButtonGroup *group = nullptr;
        QLabel *note = nullptr;
        QPushButton *button = nullptr;
        QList<core::FileChoice> files;
    };

    void showRelease(const core::GetLine &line);
    void showFile(const core::GetLine &line);
    void showWayOut(const core::GetLine &line);
    void addRow(QFormLayout *form, const QString &label, QLabel *&value, const QString &objectName);
    Choices makeChoices(const QString &name, QWidget *parent);
    // Shows `files` in `choices`; the file picked before stays picked when it is still there, else the first is.
    void setChoices(Choices &choices, const QList<core::FileChoice> &files, const QString &name);
    std::optional<core::FileChoice> picked(const Choices &choices) const;
    // A file's quality, length and peer, on two lines; `place`, once the peer has said it, stands in for the queue
    // length its search result gave.
    QString choiceText(const core::FileChoice &file, std::optional<int> place = std::nullopt) const;

    QStackedWidget *m_pages;
    QLabel *m_title;
    QLabel *m_status;
    QWidget *m_wayOut;    // under the status: stuck, or searched before a restart
    QLabel *m_wayOutNote; // what the button does, or what "Try another peer" came to
    QPushButton *m_tryAnotherPeer;
    QPushButton *m_findAgain;
    Choices m_nearMisses; // not found: the files below the quality wanted
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
    Choices m_otherFiles; // waiting in a queue: the other peers' files
    std::optional<core::GetLine> m_line;
};

} // namespace selecta::app
