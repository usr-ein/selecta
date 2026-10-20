// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/InboxInspector.h"

#include "app/Format.h"
#include "app/views/EditTagsDialog.h"
#include "app/views/ReleasePickerDialog.h"
#include "convert/TagReader.h"
#include "core/EngineHost.h"
#include "core/ReleaseJson.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QDir>
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
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString tagSourceText(const QString &source)
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
    return InboxInspector::tr("The file's own tags: no release matched it. Choose Release finds one.");
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
    if (const std::optional<convert::Picture> cover = read.tags.frontCover();
        cover && cover->data.size() <= kCoverMaxBytes) {
        QByteArray data = cover->data;
        QBuffer buffer(&data);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && size.width() <= 8000 && size.height() <= 8000) { // never decode a hostile size
            reader.setScaledSize(size.scaled(kCoverPixels, kCoverPixels, Qt::KeepAspectRatio));
            read.cover = reader.read();
        }
    }
    read.tags.clearPictures(); // the thumbnail is enough from here on
    return read;
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

    m_cover->setPixmap({});
    m_cover->setText(tr("No cover"));
    m_tags = {};
    m_tagList->setText(single ? tr("Reading…") : tr("An album folder: its files keep their own tags."));
    m_path = QDir(inboxRoot).filePath(item->relativePath);
    if (!single) {
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
    if (!read.cover.isNull()) {
        QPixmap cover = QPixmap::fromImage(read.cover);
        cover.setDevicePixelRatio(static_cast<qreal>(kCoverPixels) / kCoverPoints);
        m_cover->setPixmap(cover);
        m_cover->setText({});
    }
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

void InboxInspector::setAlsoOn(const QStringList &albums)
{
    if (albums.isEmpty()) {
        m_alsoOn->hide();
        return;
    }
    QStringList quoted;
    for (const QString &album : albums) {
        quoted.append(u"“"_s + album + u"”"_s);
    }
    m_alsoOn->setText(
        tr("⚑ Also on %1. Keep the album and move this single to the Trash, or keep both.").arg(quoted.join(u", "_s)));
    m_alsoOn->show();
}

void InboxInspector::refreshProvenance()
{
    if (!m_item) {
        return;
    }
    m_provenance->setText(tagSourceText(m_item->tagSource));
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

} // namespace selecta::app
