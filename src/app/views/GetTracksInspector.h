// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QWidget>

#include <optional>

class QButtonGroup;
class QFormLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace selecta::app {

// The release's cover at the top of the Get tracks inspector (P-021): square, as wide as the panel, like Music.app's
// info panel. Without an image it is a neutral square with a music note, the same size, so nothing below it moves when
// the cover arrives. Main thread.
class CoverArt : public QWidget
{
    Q_OBJECT

public:
    explicit CoverArt(QWidget *parent = nullptr);

    // A null image shows the placeholder.
    void setImage(const QImage &image);
    bool hasImage() const { return !m_pixmap.isNull(); }
    QSize imageSize() const { return m_pixmap.size(); } // in pixels, as decoded

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return width; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap m_pixmap;
};

// The inspector for Get tracks (PRD BATCH-9): the selected line's release, which is the tags the file will get, and
// the file chosen on Soulseek, under the release's cover (P-021). When the line needs a way out (P-004), it also offers
// one:
//  - not found, with near misses: the track's files below the quality wanted, one to pick, and "Get Anyway";
//  - waiting in a peer's queue: the other peers' files, one to pick, and "Switch to This Peer";
//  - stuck in that queue: says so plainly, with one button, "Try Another Peer" (the picked file, else the best).
// The buttons only ask (the signals); the engine decides.
//
// The cover: asked for (coverWanted()) only for the line shown, once it has been shown a moment (arrowing through the
// list asks for nothing on the way), and once per cover while the answer is awaited. The answer (showCover()) is
// untrusted bytes: checked JPEG or PNG within the cover bounds, then decoded off the main thread at most kCoverPixels
// a side. The last kCoverCacheSize covers decoded are kept in memory. No cover, a failed fetch or bytes that don't
// decode leave the placeholder; a failed cover is asked for again the next time its line is shown. Main thread.
class GetTracksInspector : public QWidget
{
    Q_OBJECT

public:
    explicit GetTracksInspector(QWidget *parent = nullptr);

    // Shows this line, or the empty state for nothing. The same line shown again keeps the file picked.
    void showLine(const std::optional<core::GetLine> &line);
    std::optional<qint64> lineId() const;

    // The cover `key` names (coverKey()), as coverWanted() asked for it: its bytes, or empty for none. A cover for a
    // line no longer shown is kept for when it is shown again.
    void showCover(const QString &key, const QByteArray &bytes);
    // What names a release's cover: the image it links to, else its MusicBrainz release, else its release group; empty
    // when it has none of them, and no cover can be found.
    static QString coverKey(const meta::Release &release);

    // Cover bytes from the network (untrusted): a JPEG or PNG within the cover bounds by its header, decoded at most
    // kCoverPixels a side. Null when they aren't one, would take too much memory to decode, or don't decode. Any thread.
    static QImage decodeCover(const QByteArray &bytes);

    static constexpr int kCoverPixels = 800;   // the largest side decoded: a 400-point panel on a Retina screen
    static constexpr int kCoverCacheSize = 12; // covers kept decoded (at most 2.5 MB each)
    static constexpr int kCoverDelayMs = 250;  // how long a line is shown before its cover is asked for
    void setCoverDelay(int ms);                // tests shorten it

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    // "Get Anyway": the picked near miss.
    void getAnywayRequested(qint64 lineId, const QString &peer, const QByteArray &remotePath);
    // "Switch to This Peer" or "Try Another Peer": the picked file; an empty peer leaves the choice to the engine
    // (the best other peer, found again if none is known).
    void switchRequested(qint64 lineId, const QString &peer, const QByteArray &remotePath);
    // "Find Again": the line's search, again (after a restart, to see what it found).
    void findAgainRequested(qint64 lineId);
    // The cover of the release shown, named `key`: showCover() gets the answer.
    void coverWanted(const QString &key, const selecta::meta::Release &release);

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
    // The cover of the line's release: from the cache, else the placeholder and, after a moment, a request.
    void showLineCover(const core::GetLine &line);
    void askForCover();
    void decoded(const QString &key, const QImage &image);
    void addRow(QFormLayout *form, const QString &label, QLabel *&value, const QString &objectName);
    Choices makeChoices(const QString &name, QWidget *parent);
    // Shows `files` in `choices`; the file picked before stays picked when it is still there, else the first is.
    void setChoices(Choices &choices, const QList<core::FileChoice> &files, const QString &name);
    std::optional<core::FileChoice> picked(const Choices &choices) const;
    // A file's quality, length and peer, on two lines; `place`, once the peer has said it, stands in for the queue
    // length its search result gave.
    QString choiceText(const core::FileChoice &file, std::optional<int> place = std::nullopt) const;

    QStackedWidget *m_pages;
    CoverArt *m_cover;
    QTimer *m_coverTimer;
    QString m_coverKey; // the cover shown or awaited; empty: the placeholder, nothing to ask for
    std::optional<meta::Release> m_coverRelease;
    QSet<QString> m_coversAsked;                // asked for, no answer yet
    QList<std::pair<QString, QImage>> m_covers; // decoded, most recently shown last
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
