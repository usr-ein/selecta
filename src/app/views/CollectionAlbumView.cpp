// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/CollectionAlbumView.h"

#include "app/Accessibility.h"
#include "app/Appearance.h"
#include "app/Format.h"
#include "app/models/CollectionWallModel.h"
#include "app/views/CollectionCovers.h"
#include "app/views/CollectionWall.h"
#include "app/views/GetTracksInspector.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QShortcut>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

// A tier's dot, for the quality column: the words beside it say it too (PRD UI-4).
QIcon tierDot(int tier, const QPalette &palette)
{
    QPixmap dot(QSize(20, 20));
    dot.setDevicePixelRatio(2.0);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(qualityTierColor(tier, palette));
    painter.drawEllipse(QRectF(2, 2, 6, 6));
    return {dot};
}

// "3", or "2-3" for a track of a second disc; empty when unnumbered.
QString trackNumberText(const core::CollectionTrack &track)
{
    if (track.trackNumber <= 0) {
        return {};
    }
    return track.discNumber > 1 ? u"%1-%2"_s.arg(track.discNumber).arg(track.trackNumber)
                                : QString::number(track.trackNumber);
}

QStandardItem *cell(const QString &text, qint64 trackId)
{
    auto *item = new QStandardItem(text);
    item->setEditable(false);
    item->setData(trackId, CollectionAlbumView::kTrackIdRole);
    return item;
}

} // namespace

CollectionAlbumView::CollectionAlbumView(CollectionCovers &covers, QWidget *parent)
    : QWidget(parent)
    , m_covers(covers)
    , m_back(new QPushButton(tr("‹ All Covers"), this))
    , m_cover(new CoverArt(this))
    , m_title(new QLabel(this))
    , m_artist(new QLabel(this))
    , m_details(new QLabel(this))
    , m_folderLabel(new QLabel(this))
    , m_model(new QStandardItemModel(this))
    , m_tracks(new QTreeView(this))
{
    setObjectName(u"collection.album"_s);
    m_back->setObjectName(u"collection.album.back"_s);
    m_back->setFlat(true);
    m_back->setToolTip(tr("Back to the covers (Esc)"));
    connect(m_back, &QPushButton::clicked, this, &CollectionAlbumView::backRequested);
    for (const QKeySequence &keys : {QKeySequence(Qt::Key_Escape), QKeySequence(QKeySequence::Back)}) {
        auto *shortcut = new QShortcut(keys, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &CollectionAlbumView::backRequested);
    }

    m_cover->setObjectName(u"collection.album.cover"_s);
    m_cover->setFixedSize(kCoverSide, kCoverSide);
    // Names from files: plain text, always.
    for (QLabel *label : {m_title, m_artist, m_details, m_folderLabel}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    m_title->setObjectName(u"collection.album.title"_s);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 6.0);
    titleFont.setWeight(QFont::Bold);
    m_title->setFont(titleFont);
    m_artist->setObjectName(u"collection.album.artist"_s);
    QFont artistFont = m_artist->font();
    artistFont.setPointSizeF(artistFont.pointSizeF() + 2.0);
    m_artist->setFont(artistFont);
    m_details->setObjectName(u"collection.album.details"_s);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    m_folderLabel->setObjectName(u"collection.album.folder"_s);
    m_folderLabel->setForegroundRole(QPalette::PlaceholderText);

    m_model->setHorizontalHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("Quality"), tr("Length")});
    m_tracks->setObjectName(u"collection.album.tracks"_s);
    m_tracks->setAccessibleName(tr("Tracks of the album"));
    m_tracks->setModel(m_model);
    m_tracks->setRootIsDecorated(false);
    m_tracks->setUniformRowHeights(true);
    m_tracks->setAlternatingRowColors(true);
    m_tracks->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tracks->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tracks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tracks->setFrameShape(QFrame::NoFrame);
    m_tracks->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tracks->header()->setStretchLastSection(false);
    m_tracks->header()->setSectionResizeMode(TitleColumn, QHeaderView::Stretch);
    m_tracks->header()->setSectionResizeMode(ArtistColumn, QHeaderView::Stretch);
    connect(m_tracks, &QWidget::customContextMenuRequested, this, &CollectionAlbumView::showMenu);
    connect(m_tracks->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &CollectionAlbumView::selectionChanged);
    a11y::addMenuKey(m_tracks);
    connect(&m_covers, &CollectionCovers::ready, this, [this](const QString &key) {
        if (key == m_coverKey) {
            showCover();
        }
    });

    auto *names = new QVBoxLayout;
    names->setSpacing(4);
    names->addStretch(1);
    names->addWidget(m_title);
    names->addWidget(m_artist);
    names->addSpacing(4);
    names->addWidget(m_details);
    names->addWidget(m_folderLabel);
    auto *top = new QHBoxLayout;
    top->setSpacing(20);
    top->addWidget(m_cover, 0, Qt::AlignTop);
    top->addLayout(names, 1);
    auto *backRow = new QHBoxLayout;
    backRow->addWidget(m_back);
    backRow->addStretch(1);
    auto *header = new QVBoxLayout;
    header->setContentsMargins(CollectionWall::kMargin - 8, 8, CollectionWall::kMargin, 12);
    header->addLayout(backRow);
    auto *indented = new QVBoxLayout;
    indented->setContentsMargins(8, 4, 0, 0);
    indented->addLayout(top);
    header->addLayout(indented);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_tracks, 1);
}

bool CollectionAlbumView::showAlbum(const std::shared_ptr<const core::CollectionListing> &listing, quint64 albumId)
{
    const core::CollectionAlbum *album = listing ? listing->album(albumId) : nullptr;
    if (album == nullptr) {
        return false;
    }
    QList<const core::CollectionTrack *> tracks;
    for (const core::CollectionTrack &track : listing->tracks) {
        if (track.albumId == albumId) {
            tracks.append(&track);
        }
    }
    // As the album plays.
    std::ranges::sort(tracks, [](const core::CollectionTrack *a, const core::CollectionTrack *b) {
        return core::playsBefore(*a, *b);
    });

    m_albumId = albumId;
    m_folder = album->folder;
    m_firstTrack = tracks.isEmpty() ? QString() : tracks.constFirst()->path;
    m_coverKey = tracks.isEmpty() ? QString() : CollectionWallModel::albumCoverKey(albumId, *tracks.constFirst());
    m_title->setText(album->title.isEmpty() ? QFileInfo(album->folder).fileName() : album->title);
    m_artist->setText(album->artist);
    QStringList details;
    if (album->year > 0) {
        details.append(QString::number(album->year));
    }
    details.append(tracks.size() == 1 ? tr("1 track") : tr("%1 tracks").arg(tracks.size()));
    qint64 length = 0;
    int below = 0;
    for (const core::CollectionTrack *track : std::as_const(tracks)) {
        length += track->durationMs;
        below += track->tier != core::QualityTier::Lossless && track->tier != core::QualityTier::Top ? 1 : 0;
    }
    if (length > 0) {
        details.append(format::length(length));
    }
    if (below > 0) {
        details.append(below == 1 ? tr("1 track below 320 kbps") : tr("%1 tracks below 320 kbps").arg(below));
    }
    m_details->setText(details.join(u" · "_s));
    m_folderLabel->setText(QDir::toNativeSeparators(album->folder));

    m_model->removeRows(0, m_model->rowCount());
    const bool severalArtists = std::ranges::any_of(
        tracks, [&tracks](const core::CollectionTrack *track) { return track->artist != tracks.constFirst()->artist; });
    for (const core::CollectionTrack *track : std::as_const(tracks)) {
        const QList<QStandardItem *> row{
            cell(trackNumberText(*track), track->id),
            cell(track->title.isEmpty() ? QFileInfo(track->path).completeBaseName() : track->title, track->id),
            cell(track->artist, track->id),
            cell(CollectionWallModel::qualityText(*track), track->id),
            cell(format::length(track->durationMs > 0 ? std::optional(track->durationMs) : std::nullopt), track->id),
        };
        row.at(QualityColumn)->setIcon(tierDot(static_cast<int>(track->tier), palette()));
        row.at(QualityColumn)->setToolTip(format::plainToolTip(core::tierName(track->tier)));
        row.at(TitleColumn)->setToolTip(format::plainToolTip(QDir::toNativeSeparators(track->path)));
        row.at(TitleColumn)->setData(track->path, kPathRole);
        row.at(NumberColumn)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.at(LengthColumn)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_model->appendRow(row);
    }
    m_tracks->setColumnHidden(ArtistColumn, !severalArtists && !tracks.isEmpty());
    m_tracks->resizeColumnToContents(NumberColumn);
    m_tracks->resizeColumnToContents(QualityColumn);
    m_tracks->resizeColumnToContents(LengthColumn);
    showCover();
    Q_EMIT selectionChanged();
    return true;
}

void CollectionAlbumView::showCover()
{
    const std::optional<QPixmap> cover = m_covers.cover(m_coverKey);
    if (!cover) {
        m_covers.request(m_coverKey, CollectionCovers::Source{m_folder, m_firstTrack});
    }
    m_cover->setImage(cover && !cover->isNull() ? cover->toImage() : QImage());
}

QList<qint64> CollectionAlbumView::selectedTracks() const
{
    QList<qint64> ids;
    const QModelIndexList rows = m_tracks->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        ids.append(row.data(kTrackIdRole).toLongLong());
    }
    std::ranges::sort(ids);
    return ids;
}

void CollectionAlbumView::showMenu(QPoint position)
{
    const QModelIndex index = m_tracks->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    const QString file = m_model->index(index.row(), TitleColumn).data(kPathRole).toString();
    QMenu menu(this);
    menu.addAction(tr("Show in Finder"), this,
                   [file] { QProcess::startDetached(u"/usr/bin/open"_s, {u"-R"_s, file}); });
    menu.exec(m_tracks->viewport()->mapToGlobal(position));
}

} // namespace selecta::app
