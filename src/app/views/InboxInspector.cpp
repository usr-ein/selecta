// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/InboxInspector.h"

#include "app/Format.h"
#include "app/Inspector.h"
#include "app/SpectrogramView.h"
#include "app/views/EditTagsDialog.h"
#include "app/views/ReleasePickerDialog.h"
#include "audio/AnalysisCache.h"
#include "convert/ArtworkFinder.h"
#include "convert/TagReader.h"
#include "core/EngineHost.h"
#include "core/ReleaseJson.h"
#include "core/SpectrumCheck.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPointer>
#include <QPromise>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QThreadPool>
#include <QVBoxLayout>

#include <array>
#include <memory>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

namespace keys = convert::keys;

constexpr int kCoverPoints = 120;
constexpr int kCoverPixels = kCoverPoints * 2; // sharp on Retina screens
constexpr qsizetype kCoverMaxBytes = qsizetype{8} * 1024 * 1024;

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
    label->setTextFormat(Qt::PlainText); // names from peers, files and tags; the tags' list is set rich, escaped
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString tagSourceText(const QString &source, bool album)
{
    if (source == u"spotify"_s) {
        return InboxInspector::tr("Tags and cover from Spotify; label and genre from MusicBrainz when Spotify has "
                                  "none.");
    }
    if (source == u"deezer"_s) {
        return InboxInspector::tr("Tags and cover from Deezer.");
    }
    if (source == u"musicbrainz"_s) {
        return InboxInspector::tr("Tags from MusicBrainz.");
    }
    if (source == u"user"_s) {
        return InboxInspector::tr("Tags edited by you.");
    }
    if (album) { // Choose Release is for singles: say nothing that points at it
        return InboxInspector::tr("No release matched the album: its files keep the tags they arrived with.");
    }
    return InboxInspector::tr("The file's own tags: no release matched it. Choose Release finds one.");
}

// A cover image (bounded), decoded at the inspector's size; null when it can't be.
QImage decodeCover(QByteArray data)
{
    if (data.size() > kCoverMaxBytes) {
        return {};
    }
    QBuffer buffer(&data);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 8000 || size.height() > 8000) { // never decode a hostile size
        return {};
    }
    reader.setScaledSize(size.scaled(kCoverPixels, kCoverPixels, Qt::KeepAspectRatio));
    return reader.read();
}

// A measured frequency, to a tenth of a kilohertz: "16.7 kHz", "22 kHz".
QString kiloHertz(int hz)
{
    return hz % 1000 == 0 ? u"%1 kHz"_s.arg(hz / 1000) : u"%1 kHz"_s.arg(hz / 1000.0, 0, 'f', 1);
}

} // namespace

InboxInspector::FileTags InboxInspector::readFileTags(const QString &path)
{
    FileTags read;
    convert::TagReadOptions options;
    options.maxPictures = 2;
    convert::Result<convert::MetadataModel> tags = convert::readTags(path, options);
    if (!tags) {
        read.error = tags.error().message;
        return read;
    }
    read.ok = true;
    read.tags = tags.takeValue();
    if (const std::optional<convert::Picture> cover = read.tags.frontCover()) {
        read.cover = decodeCover(cover->data);
    }
    read.tags.clearPictures(); // the thumbnail is enough from here on
    return read;
}

QImage InboxInspector::readAlbumCover(const QString &folder)
{
    const QDir dir(folder);
    // The folder's cover first (the arrival saves cover.jpg), chosen as the converter chooses one (CONV-5).
    const QFileInfoList images =
        dir.entryInfoList({u"*.jpg"_s, u"*.jpeg"_s, u"*.png"_s}, QDir::Files | QDir::Readable, QDir::Name);
    QList<convert::CoverCandidate> candidates;
    for (const QFileInfo &image : images) {
        candidates.append({.relativePath = image.fileName(), .size = {}});
    }
    if (const std::optional<convert::CoverChoice> choice = convert::chooseFolderCover(candidates)) {
        QFile file(images.at(choice->index).filePath());
        if (file.size() <= kCoverMaxBytes && file.open(QIODevice::ReadOnly)) {
            if (QImage cover = decodeCover(file.readAll()); !cover.isNull()) {
                return cover;
            }
        }
    }
    // Else the first track's embedded picture.
    const QFileInfoList tracks =
        dir.entryInfoList({u"*.mp3"_s, u"*.flac"_s, u"*.m4a"_s, u"*.aif"_s, u"*.aiff"_s, u"*.wav"_s, u"*.ogg"_s},
                          QDir::Files | QDir::Readable, QDir::Name);
    return tracks.isEmpty() ? QImage() : readFileTags(tracks.constFirst().filePath()).cover;
}

InboxInspector::InboxInspector(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_pages(new QStackedWidget(this))
    , m_cover(new QLabel(this))
    , m_title(new QLabel(this))
    , m_subtitle(new QLabel(this))
    , m_preview(new QPushButton(tr("Preview"), this))
    , m_edit(new QPushButton(tr("Edit Tags…"), this))
    , m_release(new QPushButton(tr("Choose Release…"), this))
    , m_tagList(body(u"inspector.inbox.tags"_s, this))
    , m_provenance(body(u"inspector.inbox.provenance"_s, this))
    , m_alsoOn(body(u"inspector.inbox.alsoOn"_s, this))
    , m_source(body(u"inspector.inbox.source"_s, this))
    , m_openRelease(new QPushButton(this))
    , m_spectrum(new QWidget(this))
    , m_spectrumText(body(u"inspector.inbox.spectrumText"_s, this))
    , m_spectrogram(new SpectrogramView(this))
    , m_spectrumNote(body(u"inspector.inbox.spectrumNote"_s, this))
{
    setObjectName(u"inspector.inbox"_s);

    auto *empty = new QLabel(tr("Select an item to see its tags, its cover and where it came from."), this);
    empty->setObjectName(u"inspector.inbox.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);

    auto *details = new QWidget;
    auto *layout = new QVBoxLayout(details);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    m_cover->setObjectName(u"inspector.inbox.cover"_s);
    m_cover->setFixedSize(kCoverPoints, kCoverPoints);
    m_cover->setAlignment(Qt::AlignCenter);
    m_cover->setFrameShape(QFrame::StyledPanel);
    m_title->setObjectName(u"inspector.inbox.title"_s);
    m_title->setWordWrap(true);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
    m_title->setFont(titleFont);
    m_subtitle->setObjectName(u"inspector.inbox.subtitle"_s);
    m_subtitle->setWordWrap(true);
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_cover);
    layout->addWidget(m_title);
    layout->addWidget(m_subtitle);

    m_preview->setObjectName(u"inspector.inbox.preview"_s);
    m_edit->setObjectName(u"inspector.inbox.edit"_s);
    m_edit->setToolTip(tr("Change the title, artists, album and other tags in the file"));
    m_release->setObjectName(u"inspector.inbox.release"_s);
    m_release->setToolTip(tr("Tag the file from another release: Spotify first, then Deezer"));
    connect(m_preview, &QPushButton::clicked, this, &InboxInspector::previewRequested);
    connect(m_edit, &QPushButton::clicked, this, [this] {
        if (!m_item) {
            return;
        }
        auto *dialog = new EditTagsDialog(QFileInfo(m_item->relativePath).fileName(), m_tags, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::accepted, this, [this, dialog, id = m_item->id] {
            const convert::MetadataModel changes = dialog->changes();
            const QStringList cleared = dialog->cleared();
            if (!changes.isEmpty() || !cleared.isEmpty()) {
                m_engine.editInboxTags(id, changes, cleared);
            }
        });
        dialog->open();
    });
    connect(m_release, &QPushButton::clicked, this, [this] {
        if (!m_item) {
            return;
        }
        const QString track = m_item->artist.isEmpty() ? m_item->title : m_item->artist + u" — "_s + m_item->title;
        auto *dialog = new ReleasePickerDialog(m_engine, m_item->id, track, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(m_preview);
    actions->addWidget(m_edit);
    actions->addStretch(1);
    layout->addLayout(actions);
    m_alsoOn->setTextFormat(Qt::PlainText); // album names from files' tags, and what the arrival noticed
    m_alsoOn->hide();
    layout->addWidget(m_alsoOn);
    layout->addWidget(m_release, 0, Qt::AlignLeft);

    layout->addWidget(heading(tr("Tags in the file"), details));
    m_tagList->setTextFormat(Qt::RichText);
    layout->addWidget(m_tagList);
    layout->addWidget(m_provenance);
    m_openRelease->setObjectName(u"inspector.inbox.openRelease"_s);
    m_openRelease->hide();
    connect(m_openRelease, &QPushButton::clicked, this, [this] {
        const std::optional<meta::Release> release =
            m_item ? core::releaseFromText(m_item->releaseJson.toUtf8()) : std::nullopt;
        if (release) {
            const QUrl url = release->spotifyUrl.isValid() ? release->spotifyUrl : release->deezerUrl;
            if (url.isValid()) {
                QDesktopServices::openUrl(url);
            }
        }
    });
    layout->addWidget(m_openRelease, 0, Qt::AlignLeft);
    layout->addWidget(heading(tr("Where it came from"), details));
    layout->addWidget(m_source);

    // PREV-9: what the spectrum of the file as downloaded says, and the spectrogram to judge it by eye.
    m_spectrum->setObjectName(u"inspector.inbox.spectrum"_s);
    m_spectrumText->setTextFormat(Qt::PlainText);
    auto *spectrumLayout = new QVBoxLayout(m_spectrum);
    spectrumLayout->setContentsMargins(0, 0, 0, 0);
    spectrumLayout->setSpacing(6);
    spectrumLayout->addWidget(heading(tr("Spectrum"), m_spectrum));
    spectrumLayout->addWidget(m_spectrumText);
    m_spectrogram->setObjectName(u"inspector.inbox.spectrogram"_s);
    m_spectrogram->setFixedHeight(120);
    spectrumLayout->addWidget(m_spectrogram);
    m_spectrumNote->setForegroundRole(QPalette::PlaceholderText);
    m_spectrumNote->setText(tr("A hard edge right across the track is typical of a lossy encoder; old recordings and "
                               "dark masters fade out gradually instead."));
    spectrumLayout->addWidget(m_spectrumNote);
    m_spectrum->hide();
    layout->addWidget(m_spectrum);
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(details);
    Inspector::fitToWidth(scroll);
    m_pages->addWidget(scroll);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);
}

void InboxInspector::setAnalysisCache(std::shared_ptr<audio::AnalysisCache> cache)
{
    m_cache = std::move(cache);
}

std::optional<qint64> InboxInspector::itemId() const
{
    return m_item ? std::optional<qint64>(m_item->id) : std::nullopt;
}

void InboxInspector::showItem(const std::optional<library::InboxItem> &item, const QString &inboxRoot)
{
    ++m_request; // a read in flight for the previous item is dropped
    m_item = item;
    if (!item) {
        m_pages->setCurrentIndex(0);
        return;
    }
    m_pages->setCurrentIndex(1);
    const bool single = item->kind == library::InboxItem::Kind::Single;
    const bool ready = item->state == library::InboxItem::State::Ready;
    const bool albumTitle = !single && !item->album.isEmpty();
    m_title->setText(albumTitle ? item->album : item->title);
    QStringList facts;
    if (!item->artist.isEmpty()) {
        facts.append(item->artist);
    }
    if (single && !item->album.isEmpty()) {
        facts.append(item->album);
    }
    if (item->year) {
        facts.append(QString::number(*item->year));
    }
    if (item->durationMs > 0) {
        facts.append(format::length(item->durationMs));
    }
    m_subtitle->setText(facts.join(u" · "_s));
    m_edit->setEnabled(single && ready);
    m_release->setEnabled(single && ready);
    m_preview->setEnabled(ready);
    refreshProvenance();
    showSpectrum();

    m_cover->setPixmap({});
    m_cover->setText(tr("No cover"));
    m_cover->setAccessibleName({}); // the text says it
    m_tags = {};
    m_tagList->setText(single ? tr("Reading…") : tr("An album folder: its files keep their own tags."));
    m_path = QDir(inboxRoot).filePath(item->relativePath);
    if (!single) {
        // Off the main thread too: the folder's cover, or its first track's.
        const auto promise = std::make_shared<QPromise<QImage>>();
        QFuture<QImage> future = promise->future();
        promise->start();
        QThreadPool::globalInstance()->start([promise, path = m_path] {
            promise->addResult(readAlbumCover(path));
            promise->finish();
        });
        future.then(this, [self = QPointer<InboxInspector>(this), request = m_request](const QImage &cover) {
            if (self && request == self->m_request) {
                self->showCover(cover);
            }
        });
        return;
    }
    // Off the main thread: a file can be slow to open (a sleeping disk).
    const auto promise = std::make_shared<QPromise<FileTags>>();
    QFuture<FileTags> future = promise->future();
    promise->start();
    QThreadPool::globalInstance()->start([promise, path = m_path] {
        promise->addResult(readFileTags(path));
        promise->finish();
    });
    future.then(this, [self = QPointer<InboxInspector>(this), request = m_request](const FileTags &read) {
        if (self && request == self->m_request) {
            self->showTags(read);
        }
    });
}

void InboxInspector::showTags(const FileTags &read)
{
    if (!read.ok) {
        m_tagList->setText(tr("Selecta can't read this file's tags: %1").arg(read.error.toHtmlEscaped()));
        return;
    }
    m_tags = read.tags;
    showCover(read.cover);
    const std::array fields{
        std::pair{keys::Title, tr("Title")},     std::pair{keys::Artist, tr("Artists")},
        std::pair{keys::Album, tr("Album")},     std::pair{keys::AlbumArtist, tr("Album artist")},
        std::pair{keys::Date, tr("Released")},   std::pair{keys::TrackNumber, tr("Track")},
        std::pair{keys::DiscNumber, tr("Disc")}, std::pair{keys::Genre, tr("Genre")},
        std::pair{keys::Label, tr("Label")},     std::pair{keys::CatalogNumber, tr("Catalogue")},
        std::pair{keys::Isrc, tr("ISRC")},       std::pair{keys::Bpm, tr("BPM")},
        std::pair{keys::InitialKey, tr("Key")},
    };
    QString rows;
    const QString labelColor = palette().color(QPalette::PlaceholderText).name();
    for (const auto &[key, label] : fields) {
        const QString value = m_tags.values(key).join(u", "_s);
        if (value.isEmpty()) {
            continue;
        }
        rows += u"<tr><td style=\"padding-right:10px; color:%1\">%2</td><td>%3</td></tr>"_s.arg(
            labelColor, label.toHtmlEscaped(), value.toHtmlEscaped());
    }
    m_tagList->setText(rows.isEmpty() ? tr("The file has no tags.") : u"<table>"_s + rows + u"</table>"_s);
}

void InboxInspector::showCover(const QImage &image)
{
    if (image.isNull()) {
        return; // "No cover" stays
    }
    QPixmap cover = QPixmap::fromImage(image);
    cover.setDevicePixelRatio(static_cast<qreal>(kCoverPixels) / kCoverPoints);
    m_cover->setPixmap(cover);
    m_cover->setText({});
    m_cover->setAccessibleName(tr("Cover"));
}

void InboxInspector::setAlsoOn(const QStringList &albums)
{
    // What the arrival found about the file itself first: its spectrum (PREV-9), its warning (DL-10, CONV-2).
    QStringList lines;
    if (const QString badge = m_item ? core::spectrumBadge(*m_item) : QString(); !badge.isEmpty()) {
        lines.append(tr("⚠ %1.").arg(badge));
    }
    if (m_item && !m_item->belowWanted.isEmpty()) {
        lines.append(tr("Below the quality you wanted (%1): you got it anyway.").arg(m_item->belowWanted));
    }
    if (m_item && !m_item->warning.isEmpty()) {
        lines.append(tr("⚠ %1").arg(m_item->warning));
    }
    if (!albums.isEmpty()) {
        QStringList quoted;
        for (const QString &album : albums) {
            quoted.append(u"“"_s + album + u"”"_s);
        }
        lines.append(tr("⚑ Also on %1. Keep the album and move this single to the Trash, or keep both.")
                         .arg(quoted.join(u", "_s)));
    }
    m_alsoOn->setText(lines.join(u"\n\n"_s));
    m_alsoOn->setVisible(!lines.isEmpty());
}

void InboxInspector::refreshProvenance()
{
    if (!m_item) {
        return;
    }
    m_provenance->setText(tagSourceText(m_item->tagSource, m_item->kind == library::InboxItem::Kind::Album));
    const std::optional<meta::Release> release = core::releaseFromText(m_item->releaseJson.toUtf8());
    QUrl link;
    if (release) {
        link = release->spotifyUrl.isValid() ? release->spotifyUrl : release->deezerUrl;
    }
    m_openRelease->setVisible(link.isValid());
    if (release && link.isValid()) {
        m_openRelease->setText(tr("Open in %1").arg(meta::sourceName(release->source)));
    }

    QStringList source;
    if (!m_item->peer.isEmpty()) {
        source.append(m_item->sourceFormat.isEmpty()
                          ? tr("Downloaded from %1.").arg(m_item->peer)
                          : tr("Downloaded from %1 as %2.").arg(m_item->peer, m_item->sourceFormat));
    }
    QString now = m_item->format;
    if (m_item->bitrate) {
        now += u' ' + QString::number(*m_item->bitrate);
    }
    if (!now.isEmpty() && !m_item->sourceFormat.isEmpty() && !m_item->sourceFormat.startsWith(now)) {
        source.append(tr("Converted to %1.").arg(now));
    }
    if (m_item->session) {
        source.append(tr("Session %1.").arg(*m_item->session));
    }
    source.append(tr("In your Inbox as %1.").arg(QFileInfo(m_item->relativePath).fileName()));
    m_source->setText(source.join(u' '));
}

void InboxInspector::showSpectrum()
{
    m_spectrogram->clear();
    const bool shown = m_item && m_item->kind == library::InboxItem::Kind::Single && m_item->qualityChecked;
    m_spectrum->setVisible(shown);
    if (!shown) {
        return;
    }
    // From what the item keeps; the cached report adds the details.
    QStringList lines;
    for (const QString &name : std::as_const(m_item->qualityFindings)) {
        if (const std::optional<audio::QualityFinding> finding = audio::findingFromName(name)) {
            lines.append(tr("⚠ %1.").arg(audio::findingBadge(*finding, m_item->qualitySourceKbps)));
        }
    }
    if (lines.isEmpty()) {
        lines.append(m_item->qualityCutoffHz
                         ? tr("✓ The spectrum reaches %1: nothing suggests a source of lower quality than it claims.")
                               .arg(kiloHertz(*m_item->qualityCutoffHz))
                         : tr("Too short, too quiet or too band-limited to judge from the spectrum."));
    } else if (m_item->qualityCutoffHz) {
        lines.append(tr("The spectrum stops at %1.").arg(kiloHertz(*m_item->qualityCutoffHz)));
    }
    m_spectrumText->setText(lines.join(u'\n'));
    m_spectrumNote->setVisible(!m_item->qualityFindings.isEmpty());
    m_spectrogram->setCutoff(m_item->qualityCutoffHz);

    const std::optional<audio::CacheKey> key = audio::CacheKey::fromHex(m_item->qualityKey);
    if (!key || !m_cache) {
        m_spectrogram->setMessage(tr("No spectrogram was kept for this file."));
        return;
    }
    m_spectrogram->setMessage(tr("Loading…"));
    // Off the main thread: a cache read is a file read and an inflate.
    const auto promise = std::make_shared<QPromise<std::optional<audio::QualityReport>>>();
    QFuture<std::optional<audio::QualityReport>> future = promise->future();
    promise->start();
    QThreadPool::globalInstance()->start([promise, cache = m_cache, key = *key] {
        promise->addResult(cache->loadQuality(key));
        promise->finish();
    });
    future.then(this, [self = QPointer<InboxInspector>(this),
                       request = m_request](const std::optional<audio::QualityReport> &report) {
        if (self && request == self->m_request) {
            self->showReport(report);
        }
    });
}

void InboxInspector::showReport(const std::optional<audio::QualityReport> &report)
{
    if (!report || !report->spectrogram) {
        m_spectrogram->setMessage(tr("The spectrogram is no longer in the cache."));
        return;
    }
    if (!report->findings.isEmpty()) {
        QStringList lines;
        for (const audio::QualityFinding finding : report->findings) {
            lines.append(tr("⚠ %1.").arg(audio::describeFinding(finding, *report)));
        }
        m_spectrumText->setText(lines.join(u'\n'));
    }
    m_spectrogram->setSpectrogram(*report->spectrogram);
    m_spectrogram->setCutoff(report->cutoffHz > 0 ? std::optional<int>(report->cutoffHz) : std::nullopt);
    if (report->spectrogram->isEmpty()) {
        m_spectrogram->setMessage(tr("No audio to show."));
    }
}

} // namespace selecta::app
