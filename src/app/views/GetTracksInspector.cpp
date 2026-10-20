// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/GetTracksInspector.h"

#include "app/Format.h"
#include "app/models/GetTracksModel.h"

#include <QDesktopServices>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
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

} // namespace

GetTracksInspector::GetTracksInspector(QWidget *parent)
    : QWidget(parent)
    , m_pages(new QStackedWidget(this))
    , m_title(new QLabel(this))
    , m_status(new QLabel(this))
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

std::optional<qint64> GetTracksInspector::lineId() const
{
    return m_line ? std::optional<qint64>(m_line->id) : std::nullopt;
}

void GetTracksInspector::showLine(const std::optional<core::GetLine> &line)
{
    m_line = line;
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

    const bool hasRelease = line->release && line->releaseState != core::GetLine::ReleaseState::None;
    m_releaseBox->setVisible(hasRelease);
    m_openRelease->setVisible(false);
    if (hasRelease) {
        const meta::Release &release = *line->release;
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
        if (line->releaseState == core::GetLine::ReleaseState::Ambiguous) {
            m_noRelease->setText(tr("Not sure this is the right release: check it before getting the file."));
            m_noRelease->setVisible(true);
        } else {
            m_noRelease->setVisible(false);
        }
    } else {
        m_releaseHeading->setText(tr("Tags it will get"));
        m_noRelease->setText(line->releaseState == core::GetLine::ReleaseState::Pending
                                 ? tr("Looking for the release on Spotify, Deezer and MusicBrainz…")
                                 : tr("No release found, so the file keeps its own tags. You can fix them in the "
                                      "Inbox."));
        m_noRelease->setVisible(true);
    }

    if (line->best) {
        const core::FileChoice &file = *line->best;
        QString text = format::quality(file);
        if (file.durationSec) {
            text += u" · "_s + format::length(qint64{*file.durationSec} * 1000);
            if (line->release && line->release->durationMs) {
                const qint64 delta = *file.durationSec - ((*line->release->durationMs + 500) / 1000);
                text += file.fitsRelease ? tr(" · same length") : u" · "_s + format::lengthDelta(delta);
            }
        }
        text +=
            u"\n"_s +
            tr("From %1 · %2").arg(file.peer, file.freeSlot ? tr("free slot") : tr("queue %1").arg(file.queueLength));
        text += u"\n"_s + QString::fromUtf8(file.remotePath);
        m_file->setText(text);
        m_fileHeading->setVisible(true);
        m_file->setVisible(true);
    } else {
        m_fileHeading->setVisible(false);
        m_file->setVisible(false);
    }
}

} // namespace selecta::app
