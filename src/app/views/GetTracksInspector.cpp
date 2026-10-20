// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/GetTracksInspector.h"

#include "app/Format.h"
#include "app/models/GetTracksModel.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QString sourceText(const meta::Release &release)
{
    return meta::sourceName(release.source);
}

QLabel *quietNote(const QString &objectName, QWidget *parent)
{
    auto *note = new QLabel(parent);
    note->setObjectName(objectName);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    return note;
}

} // namespace

GetTracksInspector::GetTracksInspector(QWidget *parent)
    : QWidget(parent)
    , m_pages(new QStackedWidget(this))
    , m_title(new QLabel(this))
    , m_status(new QLabel(this))
    , m_wayOut(new QWidget(this))
    , m_wayOutNote(new QLabel(this))
    , m_tryAnotherPeer(new QPushButton(tr("Try Another Peer"), this))
    , m_findAgain(new QPushButton(tr("Find Again"), this))
    , m_releaseBox(new QWidget(this))
    , m_openRelease(new QPushButton(this))
    , m_noRelease(new QLabel(this))
    , m_file(new QLabel(this))
{
    setObjectName(u"inspector.get"_s);

    auto *empty = new QLabel(tr("Select a track to see the tags it will get and the file chosen for it."), this);
    empty->setObjectName(u"inspector.get.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);

    auto *details = new QWidget;
    auto *layout = new QVBoxLayout(details);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    m_title->setObjectName(u"inspector.get.title"_s);
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
    m_title->setFont(titleFont);
    m_status->setObjectName(u"inspector.get.status"_s);
    m_status->setWordWrap(true);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_title);
    layout->addWidget(m_status);

    // The way out of a stuck queue (one button), or back to what a search found before a restart.
    m_wayOut->setObjectName(u"inspector.get.wayOut"_s);
    auto *wayOut = new QVBoxLayout(m_wayOut);
    wayOut->setContentsMargins(0, 0, 0, 0);
    wayOut->setSpacing(6);
    m_tryAnotherPeer->setObjectName(u"inspector.get.tryAnotherPeer"_s);
    m_tryAnotherPeer->setToolTip(tr("Leave this peer's queue and get the file from another peer that has it"));
    connect(m_tryAnotherPeer, &QPushButton::clicked, this, [this] {
        if (!m_line) {
            return;
        }
        // The file picked below, else the engine's best other peer (found again when none is known).
        const std::optional<core::FileChoice> file = picked(m_otherFiles);
        Q_EMIT switchRequested(m_line->id, file ? file->peer : QString(), file ? file->remotePath : QByteArray());
    });
    m_findAgain->setObjectName(u"inspector.get.findAgain"_s);
    connect(m_findAgain, &QPushButton::clicked, this, [this] {
        if (m_line) {
            Q_EMIT findAgainRequested(m_line->id);
        }
    });
    wayOut->addWidget(m_tryAnotherPeer, 0, Qt::AlignLeft);
    wayOut->addWidget(m_findAgain, 0, Qt::AlignLeft);
    m_wayOutNote->setObjectName(u"inspector.get.wayOut.note"_s);
    m_wayOutNote->setWordWrap(true);
    m_wayOutNote->setForegroundRole(QPalette::PlaceholderText);
    wayOut->addWidget(m_wayOutNote);
    layout->addWidget(m_wayOut);

    // Not found: the track's files below the quality wanted.
    m_nearMisses = makeChoices(u"inspector.get.nearMisses"_s, details);
    m_nearMisses.heading->setText(tr("Files below the quality wanted"));
    m_nearMisses.note->setText(tr("A lossy file is kept as it is, with its tags and cover. Your Inbox marks it below "
                                  "your quality."));
    m_nearMisses.button->setText(tr("Get Anyway"));
    m_nearMisses.button->setToolTip(tr("Get the file picked, below the quality wanted"));
    connect(m_nearMisses.button, &QPushButton::clicked, this, [this] {
        if (const std::optional<core::FileChoice> file = picked(m_nearMisses); m_line && file) {
            Q_EMIT getAnywayRequested(m_line->id, file->peer, file->remotePath);
        }
    });
    layout->addWidget(m_nearMisses.box);

    m_releaseHeading = heading(tr("Tags it will get"), details);
    layout->addWidget(m_releaseHeading);
    auto *form = new QFormLayout(m_releaseBox);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    addRow(form, tr("Title"), m_releaseTitle, u"inspector.get.release.title"_s);
    addRow(form, tr("Artist"), m_releaseArtist, u"inspector.get.release.artist"_s);
    addRow(form, tr("Album"), m_releaseAlbum, u"inspector.get.release.album"_s);
    addRow(form, tr("Released"), m_releaseDate, u"inspector.get.release.date"_s);
    addRow(form, tr("Length"), m_releaseLength, u"inspector.get.release.length"_s);
    addRow(form, tr("Track"), m_releaseTrack, u"inspector.get.release.track"_s);
    addRow(form, tr("ISRC"), m_releaseIsrc, u"inspector.get.release.isrc"_s);
    addRow(form, tr("Label"), m_releaseLabel, u"inspector.get.release.label"_s);
    layout->addWidget(m_releaseBox);
    m_openRelease->setObjectName(u"inspector.get.release.open"_s);
    connect(m_openRelease, &QPushButton::clicked, this, [this] {
        if (m_line && m_line->release) {
            const QUrl url =
                m_line->release->spotifyUrl.isValid() ? m_line->release->spotifyUrl : m_line->release->deezerUrl;
            if (url.isValid()) {
                QDesktopServices::openUrl(url);
            }
        }
    });
    layout->addWidget(m_openRelease, 0, Qt::AlignLeft);
    m_noRelease->setObjectName(u"inspector.get.noRelease"_s);
    m_noRelease->setWordWrap(true);
    m_noRelease->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_noRelease);

    m_fileHeading = heading(tr("File on Soulseek"), details);
    layout->addWidget(m_fileHeading);
    m_file->setObjectName(u"inspector.get.file"_s);
    m_file->setWordWrap(true);
    m_file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_file);

    // Waiting in a peer's queue: the other peers' files.
    m_otherFiles = makeChoices(u"inspector.get.otherFiles"_s, details);
    m_otherFiles.heading->setText(tr("Other files on Soulseek"));
    m_otherFiles.button->setText(tr("Switch to This Peer"));
    m_otherFiles.button->setToolTip(tr("Leave the queue the track waits in and get the file picked instead"));
    connect(m_otherFiles.button, &QPushButton::clicked, this, [this] {
        if (const std::optional<core::FileChoice> file = picked(m_otherFiles); m_line && file) {
            Q_EMIT switchRequested(m_line->id, file->peer, file->remotePath);
        }
    });
    layout->addWidget(m_otherFiles.box);
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(details);
    m_pages->addWidget(scroll);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);
    showLine(std::nullopt);
}

void GetTracksInspector::addRow(QFormLayout *form, const QString &label, QLabel *&value, const QString &objectName)
{
    value = new QLabel(m_releaseBox);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *name = new QLabel(label, m_releaseBox);
    name->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(name, value);
}

GetTracksInspector::Choices GetTracksInspector::makeChoices(const QString &name, QWidget *parent)
{
    Choices choices;
    choices.box = new QWidget(parent);
    choices.box->setObjectName(name);
    auto *layout = new QVBoxLayout(choices.box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    choices.heading = heading({}, choices.box);
    layout->addWidget(choices.heading);
    choices.rows = new QVBoxLayout;
    choices.rows->setContentsMargins(0, 0, 0, 0);
    choices.rows->setSpacing(6);
    layout->addLayout(choices.rows);
    choices.group = new QButtonGroup(choices.box);
    choices.group->setExclusive(true);
    choices.note = quietNote(name + u".note"_s, choices.box);
    layout->addWidget(choices.note);
    choices.button = new QPushButton(choices.box);
    choices.button->setObjectName(name + u".button"_s);
    layout->addWidget(choices.button, 0, Qt::AlignLeft);
    choices.box->setVisible(false);
    return choices;
}

void GetTracksInspector::setChoices(Choices &choices, const QList<core::FileChoice> &files, const QString &name)
{
    if (files == choices.files && choices.group->buttons().size() == files.size()) {
        return; // the same files: the one picked stays picked
    }
    const std::optional<core::FileChoice> before = picked(choices);
    while (QLayoutItem *item = choices.rows->takeAt(0)) {
        delete item->widget(); // its radio button leaves the group with it
        delete item;
    }
    choices.files = files;
    qsizetype keep = 0;
    for (qsizetype i = 0; i < files.size(); ++i) {
        const core::FileChoice &file = files.at(i);
        const QString text = choiceText(file);
        auto *row = new QWidget(choices.box);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);
        auto *radio = new QRadioButton(row);
        radio->setObjectName(u"%1.%2"_s.arg(name).arg(i));
        radio->setAccessibleName(QString(text).replace(u'\n', u", "_s));
        // The words wrap, which a radio button's own text can't: a click on them picks the file too.
        auto *label = new QLabel(text, row);
        label->setWordWrap(true);
        label->setBuddy(radio);
        label->installEventFilter(this);
        rowLayout->addWidget(radio, 0, Qt::AlignTop);
        rowLayout->addWidget(label, 1);
        choices.group->addButton(radio, static_cast<int>(i));
        choices.rows->addWidget(row);
        if (before && file.peer == before->peer && file.remotePath == before->remotePath) {
            keep = i;
        }
    }
    if (QAbstractButton *radio = choices.group->button(static_cast<int>(keep))) {
        radio->setChecked(true);
    }
}

std::optional<core::FileChoice> GetTracksInspector::picked(const Choices &choices) const
{
    const int id = choices.group != nullptr ? choices.group->checkedId() : -1;
    if (id < 0 || id >= choices.files.size()) {
        return std::nullopt;
    }
    return choices.files.at(id);
}

QString GetTracksInspector::choiceText(const core::FileChoice &file) const
{
    QString text = format::quality(file);
    if (file.durationSec) {
        text += u" · "_s + format::length(qint64{*file.durationSec} * 1000);
        if (m_line && m_line->release && m_line->release->durationMs) {
            const qint64 delta = *file.durationSec - ((*m_line->release->durationMs + 500) / 1000);
            text += file.fitsRelease ? tr(" · same length") : u" · "_s + format::lengthDelta(delta);
        }
    }
    QString from =
        tr("From %1 · %2").arg(file.peer, file.freeSlot ? tr("free slot") : tr("queue %1").arg(file.queueLength));
    if (!file.shortfall.isEmpty()) {
        from += u" · "_s + file.shortfall;
    }
    return text + u'\n' + from;
}

bool GetTracksInspector::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto *label = qobject_cast<QLabel *>(watched);
        if (auto *radio = label != nullptr ? qobject_cast<QRadioButton *>(label->buddy()) : nullptr) {
            radio->setChecked(true);
            radio->setFocus(Qt::MouseFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

std::optional<qint64> GetTracksInspector::lineId() const
{
    return m_line ? std::optional<qint64>(m_line->id) : std::nullopt;
}

void GetTracksInspector::showLine(const std::optional<core::GetLine> &line)
{
    const bool sameLine = line && m_line && line->id == m_line->id;
    m_line = line;
    if (!sameLine) {
        // Another line: nothing picked yet.
        setChoices(m_nearMisses, {}, {});
        setChoices(m_otherFiles, {}, {});
    }
    if (!line) {
        m_pages->setCurrentIndex(0);
        return;
    }
    m_pages->setCurrentIndex(1);
    m_title->setText(line->raw);
    QString status = GetTracksModel::statusText(*line);
    if (!line->message.isEmpty()) {
        status += u" · "_s + line->message;
    }
    m_status->setText(status);
    showWayOut(*line);
    showRelease(*line);
    showFile(*line);
}

void GetTracksInspector::showWayOut(const core::GetLine &line)
{
    using DownloadState = core::GetLine::DownloadState;
    const bool notFound = line.findState == core::GetLine::FindState::NotFound && line.downloadState == DownloadState::None;
    const bool waiting = line.downloadState == DownloadState::Queued;

    // Not found, but the track was there below the quality wanted: pick one, and get it anyway.
    const bool nearMisses = notFound && !line.nearMisses.isEmpty();
    setChoices(m_nearMisses, nearMisses ? line.nearMisses : QList<core::FileChoice>(), u"inspector.get.nearMiss"_s);
    m_nearMisses.box->setVisible(nearMisses);

    // Waiting in a peer's queue: the other peers' files, to switch to. Stuck, the button under the status takes the
    // file picked here.
    const bool others = waiting && !line.alternatives.isEmpty();
    setChoices(m_otherFiles, others ? line.alternatives : QList<core::FileChoice>(), u"inspector.get.other"_s);
    m_otherFiles.box->setVisible(others);
    m_otherFiles.button->setVisible(!line.stuck);
    m_otherFiles.button->setEnabled(!line.lookingForPeers);
    m_otherFiles.note->setText(line.stuck ? QString() : line.peersNote);
    m_otherFiles.note->setVisible(!m_otherFiles.note->text().isEmpty());

    const bool stuck = waiting && line.stuck;
    const bool foundBefore = notFound && line.nearMisses.isEmpty() && !line.foundThisSession;
    m_tryAnotherPeer->setVisible(stuck);
    m_tryAnotherPeer->setEnabled(!line.lookingForPeers);
    m_findAgain->setVisible(foundBefore && !stuck);
    QString note;
    if (stuck && line.lookingForPeers) {
        note = tr("Looking for another peer that has it…");
    } else if (stuck && !line.peersNote.isEmpty()) {
        note = line.peersNote;
    } else if (stuck) {
        note = others ? tr("Its queue place is given back, and the file picked below is asked for.")
                      : tr("Selecta looks for other peers that have it, then asks the best one.");
    } else if (foundBefore) {
        note = tr("Searched before Selecta last started: find it again to see what was found.");
    }
    m_wayOutNote->setText(note);
    m_wayOutNote->setVisible(!note.isEmpty());
    m_wayOut->setVisible(stuck || foundBefore);
}

void GetTracksInspector::showRelease(const core::GetLine &line)
{
    const bool hasRelease = line.release && line.releaseState != core::GetLine::ReleaseState::None;
    m_releaseBox->setVisible(hasRelease);
    m_openRelease->setVisible(false);
    if (!hasRelease) {
        m_releaseHeading->setText(tr("Tags it will get"));
        m_noRelease->setText(line.releaseState == core::GetLine::ReleaseState::Pending
                                 ? tr("Looking for the release on Spotify, Deezer and MusicBrainz…")
                                 : tr("No release found, so the file keeps its own tags. You can fix them in the "
                                      "Inbox."));
        m_noRelease->setVisible(true);
        return;
    }
    const meta::Release &release = *line.release;
    m_releaseHeading->setText(tr("Tags it will get · %1").arg(sourceText(release)));
    m_releaseTitle->setText(release.title);
    m_releaseArtist->setText(release.artist());
    m_releaseAlbum->setText(release.album);
    m_releaseDate->setText(release.releaseDate);
    m_releaseLength->setText(format::length(release.durationMs));
    QString track;
    if (release.trackNumber && release.trackTotal) {
        track = tr("%1 of %2").arg(*release.trackNumber).arg(*release.trackTotal);
    } else if (release.trackNumber) {
        track = QString::number(*release.trackNumber);
    }
    m_releaseTrack->setText(track);
    m_releaseIsrc->setText(release.isrc);
    m_releaseLabel->setText(release.label.isEmpty() ? tr("from MusicBrainz on arrival") : release.label);
    // Spotify's terms ask for a link back next to its data (Developer Policy §II.4).
    if (release.spotifyUrl.isValid() && !release.spotifyUrl.isEmpty()) {
        m_openRelease->setText(tr("Open in Spotify"));
        m_openRelease->setVisible(true);
    } else if (release.deezerUrl.isValid() && !release.deezerUrl.isEmpty()) {
        m_openRelease->setText(tr("Open in Deezer"));
        m_openRelease->setVisible(true);
    }
    const bool unsure = line.releaseState == core::GetLine::ReleaseState::Ambiguous;
    m_noRelease->setText(unsure ? tr("Not sure this is the right release: check it before getting the file.")
                                : QString());
    m_noRelease->setVisible(unsure);
}

void GetTracksInspector::showFile(const core::GetLine &line)
{
    m_fileHeading->setVisible(line.best.has_value());
    m_file->setVisible(line.best.has_value());
    if (!line.best) {
        return;
    }
    const core::FileChoice &file = *line.best;
    QString text = choiceText(file);
    if (line.placeInQueue && line.downloadState == core::GetLine::DownloadState::Queued) {
        text += tr(" · your place %1").arg(*line.placeInQueue);
    }
    text += u"\n"_s + QString::fromUtf8(file.remotePath);
    // Waiting in a queue, the other files are offered below to switch to; otherwise they are listed here.
    if (!line.alternatives.isEmpty() && line.downloadState != core::GetLine::DownloadState::Queued) {
        text += u"\n\n"_s + tr("Other files:");
        for (const core::FileChoice &other : line.alternatives) {
            QString alternative = format::quality(other);
            if (other.durationSec) {
                alternative += u" · "_s + format::length(qint64{*other.durationSec} * 1000);
            }
            text +=
                u"\n"_s + tr("%1 · from %2%3")
                              .arg(alternative, other.peer, other.fitsRelease ? tr(" · fits the release") : QString());
        }
    }
    m_file->setText(text);
}

} // namespace selecta::app
