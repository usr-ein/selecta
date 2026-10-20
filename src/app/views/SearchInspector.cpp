// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/SearchInspector.h"

#include "app/Format.h"
#include "app/models/SearchResultsModel.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <cstdlib>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kSameRecordingSeconds = 3;

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QLabel *body(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QString ownershipNote(const core::SearchResult &result)
{
    // The file's name says enough; the whole path is in the tooltip.
    const QString detail =
        result.ownershipDetail.contains(u'/') ? QFileInfo(result.ownershipDetail).fileName() : result.ownershipDetail;
    switch (result.ownership) {
    case core::Ownership::InCollection:
        return SearchInspector::tr("In your collection: %1. Downloading it again asks first.").arg(detail);
    case core::Ownership::Upgrade:
        return SearchInspector::tr("An upgrade: you have %1 in lower quality.").arg(detail);
    case core::Ownership::LikelyInCollection:
        return SearchInspector::tr("Probably in your collection: %1. The version isn't clear from the names.")
            .arg(detail);
    case core::Ownership::InInbox:
        return SearchInspector::tr("In your Inbox, not filed yet.");
    case core::Ownership::Queued:
        return SearchInspector::tr("In your transfers.");
    case core::Ownership::DownloadedThisSession:
        return SearchInspector::tr("Downloaded this session (%1).").arg(detail);
    case core::Ownership::DownloadedBefore:
        return SearchInspector::tr("Downloaded before (%1).").arg(detail);
    case core::Ownership::OtherVersion:
        return SearchInspector::tr("You have another version of it: %1.").arg(detail);
    case core::Ownership::None:
        break;
    }
    return SearchInspector::tr("Not in your collection, Inbox or download history.");
}

} // namespace

SearchInspector::SearchInspector(QWidget *parent)
    : QWidget(parent)
    , m_pages(new QStackedWidget(this))
    , m_title(new QLabel(this))
    , m_subtitle(new QLabel(this))
    , m_preview(new QPushButton(tr("Preview"), this))
    , m_download(new QPushButton(tr("Download"), this))
    , m_downloadFolder(new QPushButton(tr("Download Folder as Album…"), this))
    , m_ownership(body(u"inspector.search.ownership"_s, this))
    , m_checks(body(u"inspector.search.checks"_s, this))
    , m_quality(body(u"inspector.search.quality"_s, this))
    , m_file(body(u"inspector.search.file"_s, this))
    , m_peer(body(u"inspector.search.peer"_s, this))
{
    setObjectName(u"inspector.search"_s);

    auto *empty = new QLabel(
        tr("Select a result to see its quality check, what you already have, and how much you took from its peer."),
        this);
    empty->setObjectName(u"inspector.search.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);

    auto *details = new QWidget;
    auto *layout = new QVBoxLayout(details);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    m_title->setObjectName(u"inspector.search.title"_s);
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_title->setTextFormat(Qt::PlainText);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
    m_title->setFont(titleFont);
    m_subtitle->setObjectName(u"inspector.search.subtitle"_s);
    m_subtitle->setWordWrap(true);
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_title);
    layout->addWidget(m_subtitle);

    m_preview->setObjectName(u"inspector.search.preview"_s);
    m_preview->setToolTip(tr("Streams this file from the peer into the preview cache (Space)"));
    m_download->setObjectName(u"inspector.search.download"_s);
    m_download->setToolTip(tr("Download it to the Inbox, converted and tagged (Return)"));
    m_downloadFolder->setObjectName(u"inspector.search.downloadFolder"_s);
    m_downloadFolder->setToolTip(tr("Download the whole folder from this peer as one album in the Inbox"));
    connect(m_preview, &QPushButton::clicked, this, &SearchInspector::previewRequested);
    connect(m_download, &QPushButton::clicked, this, &SearchInspector::downloadRequested);
    connect(m_downloadFolder, &QPushButton::clicked, this, &SearchInspector::downloadFolderRequested);
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(m_preview);
    actions->addWidget(m_download);
    actions->addStretch(1);
    layout->addLayout(actions);
    auto *folderActions = new QHBoxLayout; // a line of its own: the panel is narrow
    folderActions->setContentsMargins(0, 0, 0, 0);
    folderActions->addWidget(m_downloadFolder);
    folderActions->addStretch(1);
    layout->addLayout(folderActions);

    layout->addWidget(m_ownership);
    layout->addWidget(m_checks);
    layout->addWidget(heading(tr("Quality"), details));
    layout->addWidget(m_quality);
    layout->addWidget(heading(tr("File"), details));
    layout->addWidget(m_file);
    layout->addWidget(heading(tr("Peer"), details));
    layout->addWidget(m_peer);
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(details);
    m_pages->addWidget(scroll);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);
}

std::optional<int> SearchInspector::resultIndex() const
{
    return m_index;
}

void SearchInspector::showResult(const std::optional<core::SearchResult> &result, qint64 releaseDurationMs)
{
    if (!result) {
        m_index.reset();
        m_pages->setCurrentIndex(0);
        return;
    }
    m_index = result->index;
    m_pages->setCurrentIndex(1);
    const match::ParsedRemote &parsed = result->parsed;
    const QString title = parsed.title.value.isEmpty() ? result->name : parsed.title.value;
    m_title->setText(title);
    QStringList facts;
    if (!parsed.artist.value.isEmpty()) {
        facts.append(parsed.artist.value);
    }
    facts.append(SearchResultsModel::qualityText(*result));
    if (result->durationSec() > 0) {
        facts.append(format::length(qint64{result->durationSec()} * 1000));
    }
    facts.append(format::size(static_cast<qint64>(result->hit.size)));
    m_subtitle->setText(facts.join(u" · "_s));
    m_preview->setEnabled(result->isAudio());
    m_download->setEnabled(result->isAudio());
    m_downloadFolder->setEnabled(result->isAudio() && !result->folder.isEmpty());

    m_ownership->setText(ownershipNote(*result));
    m_ownership->setToolTip(result->ownershipDetail);

    QStringList checks;
    if (releaseDurationMs > 0 && result->durationSec() > 0) {
        const qint64 delta = result->durationSec() - ((releaseDurationMs + 500) / 1000);
        checks.append(
            std::abs(delta) <= kSameRecordingSeconds
                ? tr("✓ Same length as the release (%1).").arg(format::lengthDelta(delta))
                : tr("⚠ %1 against the release: probably another version or track.").arg(format::lengthDelta(delta)));
    } else if (result->fit && result->fit->lengthUnknown) {
        checks.append(tr("Its length isn't known, so only its name was compared with the release."));
    }
    if (!result->quality.plausible) {
        checks.append(tr("⚠ Its size suggests about %1 kbps: not the quality it claims.")
                          .arg(result->quality.estimatedBitrateKbps));
    } else if (!result->quality.established) {
        checks.append(tr("The peer didn't report its quality."));
    } else {
        checks.append(tr("✓ Its size matches the quality it claims."));
    }
    m_checks->setText(checks.join(u'\n'));

    const match::AudioQuality &declared = result->quality.quality;
    QStringList quality;
    quality.append(tr("Format: %1").arg(result->hit.extension.toUpper()));
    if (declared.bitDepth > 0 || declared.sampleRateHz > 0) {
        quality.append(
            tr("Resolution: %1-bit · %2 kHz").arg(declared.bitDepth).arg(declared.sampleRateHz / 1000.0, 0, 'g', 4));
    }
    if (declared.bitrateKbps > 0) {
        quality.append(tr("Bitrate: %1 kbps%2")
                           .arg(declared.bitrateKbps)
                           .arg(declared.vbr.value_or(false) ? tr(" (VBR)") : QString()));
    }
    if (result->quality.estimatedBitrateKbps > 0) {
        quality.append(tr("From its size: ≈ %1 kbps").arg(result->quality.estimatedBitrateKbps));
    }
    m_quality->setText(quality.join(u'\n'));

    QStringList file{tr("Name: %1").arg(result->name), tr("Folder: %1").arg(result->folder)};
    if (!parsed.title.value.isEmpty()) {
        QString readAs =
            parsed.artist.value.isEmpty() ? parsed.title.value : parsed.artist.value + u" · "_s + parsed.title.value;
        if (parsed.track.isSet()) {
            readAs += tr(" · track %1").arg(parsed.track.value);
        }
        file.append(tr("Read as: %1").arg(readAs));
    }
    m_file->setText(file.join(u'\n'));

    QStringList peer{result->hit.peer};
    peer.append(result->hit.freeSlot ? tr("Upload slot: free now")
                                     : tr("Upload slot: busy, %1 waiting").arg(result->hit.queueLength));
    if (const QString speed = format::speed(result->hit.averageSpeed); !speed.isEmpty()) {
        peer.append(tr("Speed: %1").arg(speed));
    }
    peer.append(result->peerFilesToday == 1 ? tr("From them today: 1 file")
                                            : tr("From them today: %1 files").arg(result->peerFilesToday));
    if (result->peerBackoffMs > 0) {
        peer.append(tr("Holding off: they refused a download lately. Selecta waits %1 more minutes before asking "
                       "again.")
                        .arg((result->peerBackoffMs + 59'999) / 60'000));
    }
    m_peer->setText(peer.join(u'\n'));
}

} // namespace selecta::app
