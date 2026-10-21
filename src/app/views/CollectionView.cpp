// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/CollectionView.h"

#include "app/Format.h"
#include "app/Icons.h"
#include "app/models/CollectionWallModel.h"
#include "app/views/CollectionAlbumView.h"
#include "app/views/CollectionCovers.h"
#include "app/views/CollectionWall.h"
#include "app/views/EmptyStateView.h"
#include "core/EngineHost.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kWallPage = 0;
constexpr int kAlbumPage = 1;

// A folder as the Finder says it: the home folder as "~".
QString shownFolder(const QString &path)
{
    const QString home = QDir::homePath();
    if (!home.isEmpty() && home != u"/" && (path == home || path.startsWith(home + u'/'))) {
        return QDir::toNativeSeparators(u"~"_s + path.mid(home.size()));
    }
    return QDir::toNativeSeparators(path);
}

QString tracksText(int count)
{
    return count == 1 ? CollectionView::tr("1 track") : CollectionView::tr("%1 tracks").arg(QLocale().toString(count));
}

} // namespace

CollectionView::CollectionView(core::EngineHost &engine, storage::Database & /*db*/, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_listing(std::make_shared<const core::CollectionListing>())
    , m_covers(new CollectionCovers(this))
    , m_pages(new QStackedWidget(this))
    , m_headline(new QLabel(this))
    , m_order(new QComboBox(this))
    , m_details(new QLabel(this))
    , m_folders(new QLabel(this))
    , m_message(new QLabel(this))
    , m_progress(new QProgressBar(this))
    , m_scan(new QPushButton(tr("Scan Now"), this))
    , m_unreadableTitle(new QLabel(this))
    , m_unreadable(new QListWidget(this))
    , m_flagsTitle(new QLabel(this))
    , m_flags(new QListWidget(this))
    , m_formats(new QLabel(this))
    , m_inboxRow(new QWidget(this))
    , m_inbox(new QLabel(this))
    , m_openInbox(new QPushButton(tr("Open Inbox"), this))
    , m_browse(new QStackedWidget(this))
    , m_wallModel(new CollectionWallModel(this))
    , m_wall(new CollectionWall(*m_covers, this))
    , m_album(new CollectionAlbumView(*m_covers, this))
    , m_result(new QLabel(this))
    , m_selection(new QLabel(this))
    , m_betterQuality(new QPushButton(tr("Search for Better Quality"), this))
{
    setObjectName(u"view.collection"_s);

    // No folders yet: the one thing to do.
    auto *empty = new QWidget(m_pages);
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->addWidget(new EmptyStateView(icons::symbol(u"music.note.list"_s, u"folder-music"_s),
                                              tr("No collection yet"),
                                              tr("Tell Selecta where your music is, and Get tracks will know what "
                                                 "you already have. It reads those folders and never changes a file "
                                                 "in them."),
                                              {}, empty),
                           1);
    auto *choose = new QPushButton(tr("Choose Folders…"), empty);
    choose->setObjectName(u"collection.choose"_s);
    choose->setDefault(true);
    connect(choose, &QPushButton::clicked, this, &CollectionView::settingsRequested);
    auto *chooseRow = new QHBoxLayout;
    chooseRow->addStretch(1);
    chooseRow->addWidget(choose);
    chooseRow->addStretch(1);
    emptyLayout->addLayout(chooseRow);
    emptyLayout->addSpacing(48);
    m_pages->addWidget(empty);

    // The collection.
    auto *page = new QWidget(m_pages);
    m_headline->setObjectName(u"collection.headline"_s);
    QFont headlineFont = m_headline->font();
    headlineFont.setPointSizeF(headlineFont.pointSizeF() + 3.0);
    headlineFont.setWeight(QFont::DemiBold);
    m_headline->setFont(headlineFont);
    m_order->setObjectName(u"collection.order"_s);
    m_order->setAccessibleName(tr("Group the covers by"));
    m_order->addItem(tr("Date Downloaded"), static_cast<int>(core::CollectionOrder::Added));
    m_order->addItem(tr("Artist"), static_cast<int>(core::CollectionOrder::Artist));
    m_order->addItem(tr("Genre"), static_cast<int>(core::CollectionOrder::Genre));
    m_order->addItem(tr("Quality"), static_cast<int>(core::CollectionOrder::Quality));
    m_order->setToolTip(tr("Group the covers by month downloaded (newest first; a file Selecta didn't download goes by "
                           "its date), by artist, by genre, or by quality (worst first)"));
    connect(m_order, &QComboBox::currentIndexChanged, this, [this] {
        const QList<qint64> tracks = selectedTracks();
        const QSet<qint64> selected(tracks.cbegin(), tracks.cend());
        m_wallModel->setOrder(static_cast<core::CollectionOrder>(m_order->currentData().toInt()));
        for (const int row : m_wallModel->rowsWithAny(selected)) {
            m_wall->selectionModel()->select(m_wallModel->index(row), QItemSelectionModel::Select);
        }
        m_wall->verticalScrollBar()->setValue(0);
    });
    auto *orderLabel = new QLabel(tr("Group by"), this);
    orderLabel->setBuddy(m_order);
    m_details->setObjectName(u"collection.details"_s);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_folders->setObjectName(u"collection.folders"_s);
    m_folders->setTextFormat(Qt::PlainText); // folder names
    m_folders->setForegroundRole(QPalette::PlaceholderText);
    m_folders->setWordWrap(true);
    m_folders->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setObjectName(u"collection.message"_s);
    m_message->setTextFormat(Qt::PlainText); // may name a file or folder
    m_message->setWordWrap(true);
    m_message->hide();
    m_progress->setObjectName(u"collection.progress"_s);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(6);
    m_progress->hide();
    m_scan->setObjectName(u"collection.scan"_s);
    m_scan->setToolTip(tr("Look through the folders again now. Changes are also picked up as they happen."));
    connect(m_scan, &QPushButton::clicked, &m_engine, &core::EngineHost::scanCollection);
    auto *folders = new QPushButton(tr("Folders…"), page);
    folders->setObjectName(u"collection.folders.choose"_s);
    connect(folders, &QPushButton::clicked, this, &CollectionView::settingsRequested);
    m_unreadableTitle->setObjectName(u"collection.unreadableTitle"_s);
    m_unreadable->setObjectName(u"collection.unreadable"_s);
    m_unreadable->setAccessibleName(tr("Files that couldn't be read"));
    m_unreadable->setSelectionMode(QAbstractItemView::NoSelection);
    m_unreadable->setFrameShape(QFrame::NoFrame);
    m_unreadable->setMaximumHeight(110);
    m_formats->setObjectName(u"collection.formats"_s);
    m_formats->setTextFormat(Qt::PlainText); // the files' types
    m_formats->setWordWrap(true);
    m_formats->setForegroundRole(QPalette::PlaceholderText);
    m_inbox->setObjectName(u"collection.inbox"_s);
    m_openInbox->setObjectName(u"collection.openInbox"_s);
    connect(m_openInbox, &QPushButton::clicked, this, &CollectionView::inboxRequested);
    m_flagsTitle->setObjectName(u"collection.flagsTitle"_s);
    m_flagsTitle->setWordWrap(true);
    m_flags->setObjectName(u"collection.flags"_s);
    m_flags->setAccessibleName(tr("Singles also on an album"));
    m_flags->setFrameShape(QFrame::NoFrame);
    m_flags->setContextMenuPolicy(Qt::CustomContextMenu);
    m_flags->setMaximumHeight(110);
    connect(m_flags, &QListWidget::customContextMenuRequested, this, [this](QPoint position) {
        const QListWidgetItem *item = m_flags->itemAt(position);
        if (!item) {
            return;
        }
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        const QString path = item->data(Qt::UserRole + 1).toString();
        QMenu menu(this);
        menu.addAction(tr("Show in Finder"), this,
                       [path] { QProcess::startDetached(u"/usr/bin/open"_s, {u"-R"_s, path}); });
        // LIB-6: remembered, so the pair is never flagged again; Dismiss only hides this flag.
        menu.addAction(tr("Not the Same Track"), this, [this, id] { m_engine.notTheSameTrack(id); });
        menu.addAction(tr("Dismiss"), this, [this, id] { m_engine.dismissSupersedeFlag(id); });
        menu.exec(m_flags->viewport()->mapToGlobal(position));
    });

    // The wall, or an album opened from it.
    m_wall->setObjectName(u"collection.wall"_s);
    m_wall->setAccessibleName(tr("Covers"));
    m_wall->setModel(m_wallModel);
    connect(m_wall, &CollectionWall::albumOpened, this, &CollectionView::openAlbum);
    connect(m_wall->selectionModel(), &QItemSelectionModel::selectionChanged, this, &CollectionView::refreshSelection);
    connect(m_wall, &QWidget::customContextMenuRequested, this, &CollectionView::showWallMenu);
    connect(m_album, &CollectionAlbumView::backRequested, this, &CollectionView::closeAlbum);
    connect(m_album, &CollectionAlbumView::selectionChanged, this, &CollectionView::refreshSelection);
    m_browse->addWidget(m_wall);  // kWallPage
    m_browse->addWidget(m_album); // kAlbumPage

    // The selection, and what can be done with it.
    m_result->setObjectName(u"collection.result"_s);
    m_result->setTextFormat(Qt::PlainText);
    m_result->setWordWrap(true);
    m_selection->setObjectName(u"collection.selection"_s);
    m_selection->setForegroundRole(QPalette::PlaceholderText);
    m_betterQuality->setObjectName(u"collection.betterQuality"_s);
    connect(m_betterQuality, &QPushButton::clicked, this, [this] { searchForBetterQuality(selectedTracks()); });
    connect(this, &CollectionView::betterQualityRequested, &m_engine, &core::EngineHost::searchForBetterQuality);
    connect(&m_engine, &core::EngineHost::betterQualitySearched, this, &CollectionView::showSearched);

    auto *top = new QHBoxLayout;
    top->addWidget(m_headline, 1);
    top->addWidget(orderLabel);
    top->addWidget(m_order);
    top->addSpacing(12);
    top->addWidget(folders);
    top->addWidget(m_scan);
    auto *inboxLayout = new QHBoxLayout(m_inboxRow);
    inboxLayout->setContentsMargins(0, 0, 0, 0);
    inboxLayout->addWidget(m_inbox, 1);
    inboxLayout->addWidget(m_openInbox);
    auto *header = new QVBoxLayout;
    header->setContentsMargins(24, 20, 24, 4);
    header->setSpacing(6);
    header->addLayout(top);
    header->addWidget(m_details);
    header->addWidget(m_folders);
    header->addWidget(m_formats);
    header->addWidget(m_progress);
    header->addWidget(m_message);
    header->addWidget(m_inboxRow);
    header->addWidget(m_flagsTitle);
    header->addWidget(m_flags);
    header->addWidget(m_unreadableTitle);
    header->addWidget(m_unreadable);
    auto *line = new QFrame(page);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setForegroundRole(QPalette::Mid);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(24, 8, 24, 12);
    bar->addWidget(m_result, 1);
    bar->addWidget(m_selection);
    bar->addWidget(m_betterQuality);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_browse, 1);
    layout->addWidget(line);
    layout->addLayout(bar);
    m_pages->addWidget(page);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);

    connect(&m_engine, &core::EngineHost::collectionStatusChanged, this, &CollectionView::show);
    connect(&m_engine, &core::EngineHost::collectionListingChanged, this, &CollectionView::showListing);
    connect(&m_engine, &core::EngineHost::supersedeFlagsChanged, this, &CollectionView::showFlags);
    connect(&m_engine, &core::EngineHost::inboxChanged, this, &CollectionView::showInbox);
    show(m_engine.collectionStatus());
    showListing(m_engine.collectionListing());
    showFlags(m_engine.supersedeFlags());
    showInbox();
}

CollectionView::~CollectionView() = default;

void CollectionView::showInbox()
{
    const QList<core::InboxEntry> entries = m_engine.inboxEntries();
    m_inbox->setText(entries.size() == 1 ? tr("1 item waiting in the Inbox to be filed.")
                                         : tr("%1 items waiting in the Inbox to be filed.").arg(entries.size()));
    m_inboxRow->setVisible(!entries.isEmpty());
}

void CollectionView::showFlags(const QList<core::SupersedeFlag> &flags)
{
    m_flags->clear();
    for (const core::SupersedeFlag &flag : flags) {
        if (flag.singlePath.isEmpty()) {
            continue; // Inbox singles show their flag in the Inbox
        }
        auto *item = new QListWidgetItem(tr("⚑ %1 is also on “%2”").arg(flag.single, flag.album), m_flags);
        item->setData(Qt::UserRole, flag.id);
        item->setData(Qt::UserRole + 1, flag.singlePath);
        item->setToolTip(format::plainToolTip(QDir::toNativeSeparators(flag.singlePath) + u'\n' + flag.summary));
    }
    const int count = m_flags->count();
    m_flagsTitle->setText(count == 1 ? tr("1 single in your collection is also on an album. Selecta never changes "
                                          "it: keep both, or remove the single yourself.")
                                     : tr("%1 singles in your collection are also on an album. Selecta never "
                                          "changes them: keep both, or remove the singles yourself.")
                                           .arg(count));
    m_flagsTitle->setVisible(count > 0);
    m_flags->setVisible(count > 0);
}

QString CollectionView::summary() const
{
    if (m_status.roots.isEmpty()) {
        return tr("No folders indexed");
    }
    if (m_status.scanning) {
        return tr("Scanning…");
    }
    return tracksText(m_status.tracks);
}

void CollectionView::show(const core::CollectionStatus &status)
{
    m_status = status;
    QStringList formats;
    for (const auto &[format, count] : status.formats) {
        formats.append(tr("%1: %2").arg(format, QLocale().toString(count)));
    }
    m_formats->setText(formats.join(u" · "_s));
    m_formats->setVisible(!formats.isEmpty());
    m_pages->setCurrentIndex(status.roots.isEmpty() ? 0 : 1);
    const QLocale locale;
    m_headline->setText(tracksText(status.tracks));

    QStringList details;
    if (status.albums > 0) {
        details.append(status.albums == 1 ? tr("1 album") : tr("%1 albums").arg(locale.toString(status.albums)));
    }
    if (status.bytes > 0) {
        details.append(format::size(status.bytes));
    }
    if (status.scanning) {
        details.append(status.total > 0 ? tr("reading %1 of %2 new or changed files")
                                              .arg(locale.toString(status.done), locale.toString(status.total))
                                        : tr("looking through the folders"));
    } else if (status.lastScanMs > 0) {
        details.append(
            tr("last scanned %1")
                .arg(locale.toString(QDateTime::fromMSecsSinceEpoch(status.lastScanMs), QLocale::ShortFormat)));
    }
    m_details->setText(details.join(u" · "_s));
    m_details->setVisible(!details.isEmpty());
    // Each folder by its role (PRD LIB-1).
    const auto folder = [](const QString &path) { return path.isEmpty() ? tr("not chosen") : shownFolder(path); };
    m_folders->setText(
        tr("Tracks folder: %1 · Albums folder: %2").arg(folder(status.tracksFolder), folder(status.albumsFolder)));
    QStringList chosen;
    for (const QString &path : {status.tracksFolder, status.albumsFolder}) {
        if (!path.isEmpty()) {
            chosen.append(QDir::toNativeSeparators(path));
        }
    }
    m_folders->setToolTip(format::plainToolTip(chosen.join(u'\n'))); // folder names

    m_progress->setVisible(status.scanning);
    m_progress->setRange(0, status.total > 0 ? status.total : 0); // 0…0: busy
    m_progress->setValue(status.done);
    m_scan->setEnabled(!status.scanning);
    m_message->setText(status.message);
    m_message->setVisible(!status.message.isEmpty());

    m_unreadable->clear();
    for (const auto &[path, reason] : status.unreadableSample) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(path) + u"\n"_s + reason, m_unreadable);
        item->setToolTip(format::plainToolTip(reason));
    }
    m_unreadableTitle->setText(status.unreadable == 1
                                   ? tr("1 file couldn't be read")
                                   : tr("%1 files couldn't be read").arg(locale.toString(status.unreadable)));
    m_unreadableTitle->setVisible(status.unreadable > 0);
    m_unreadable->setVisible(status.unreadable > 0);
    showEmptyText();
    Q_EMIT summaryChanged();
}

void CollectionView::showEmptyText()
{
    m_wall->setEmptyText(m_status.scanning ? tr("Reading your folders…") : tr("No tracks in these folders yet."));
}

void CollectionView::showListing(const std::shared_ptr<const core::CollectionListing> &listing)
{
    if (!listing) {
        return;
    }
    // What was selected stays selected, where it is now; the wall stays where it was.
    const QList<qint64> before = selectedTracks();
    const QSet<qint64> selected(before.cbegin(), before.cend());
    const int scroll = m_wall->verticalScrollBar()->value();
    m_listing = listing;
    m_tiers.clear();
    m_tiers.reserve(listing->tracks.size());
    for (const core::CollectionTrack &track : listing->tracks) {
        m_tiers.insert(track.id, track.tier);
    }
    m_covers->clear(); // a file may have changed, an upgrade taken its place
    m_wallModel->setListing(listing);
    m_wall->doItemsLayout();
    for (const int row : m_wallModel->rowsWithAny(selected)) {
        m_wall->selectionModel()->select(m_wallModel->index(row), QItemSelectionModel::Select);
    }
    m_wall->verticalScrollBar()->setValue(scroll);
    if (m_browse->currentIndex() == kAlbumPage && !m_album->showAlbum(listing, m_album->albumId())) {
        closeAlbum(); // gone from the collection
    }
    refreshSelection();
}

void CollectionView::openAlbum(quint64 albumId)
{
    if (m_album->showAlbum(m_listing, albumId)) {
        m_browse->setCurrentIndex(kAlbumPage);
        m_album->trackList()->setFocus();
        refreshSelection();
    }
}

void CollectionView::closeAlbum()
{
    const quint64 album = m_album->albumId();
    m_browse->setCurrentIndex(kWallPage);
    if (const int row = m_wallModel->rowOfAlbum(album); row >= 0) {
        m_wall->setCurrentIndex(m_wallModel->index(row));
        m_wall->scrollTo(m_wallModel->index(row));
    }
    m_wall->setFocus();
    refreshSelection();
}

QList<qint64> CollectionView::selectedTracks() const
{
    if (m_browse->currentIndex() == kAlbumPage) {
        return m_album->selectedTracks();
    }
    return m_wallModel->tracksOf(m_wall->selectionModel()->selectedIndexes());
}

QList<qint64> CollectionView::upgradable(const QList<qint64> &tracks) const
{
    QList<qint64> result;
    for (const qint64 id : tracks) {
        if (m_tiers.value(id, core::QualityTier::Lossless) != core::QualityTier::Lossless) {
            result.append(id);
        }
    }
    return result;
}

void CollectionView::refreshSelection()
{
    const QList<qint64> tracks = selectedTracks();
    const QList<qint64> better = upgradable(tracks);
    if (tracks.isEmpty()) {
        m_selection->clear();
    } else if (m_browse->currentIndex() == kWallPage) {
        const auto tiles = static_cast<int>(m_wall->selectionModel()->selectedIndexes().size());
        m_selection->setText(tiles == static_cast<int>(tracks.size())
                                 ? tracksText(static_cast<int>(tracks.size())) + tr(" selected")
                                 : tr("%1 selected · %2").arg(tiles).arg(tracksText(static_cast<int>(tracks.size()))));
    } else {
        m_selection->setText(tracksText(static_cast<int>(tracks.size())) + tr(" selected"));
    }
    m_betterQuality->setEnabled(!better.isEmpty());
    if (tracks.isEmpty()) {
        m_betterQuality->setToolTip(tr("Select tracks or albums first: Selecta looks for a better copy of each one, and "
                                       "a copy it finds takes the old file's place once you file it from the Inbox"));
    } else if (better.isEmpty()) {
        m_betterQuality->setToolTip(tr("Everything selected is from lossless already: nothing is better"));
    } else {
        m_betterQuality->setToolTip(
            tr("Look for a better copy of each track selected, in Get tracks. What it finds arrives in the Inbox "
               "marked Upgrade; filed, it takes the old file's place, and the old file goes to the Trash"));
    }
}

void CollectionView::searchForBetterQuality(const QList<qint64> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }
    m_result->setText(tr("Asking Get tracks for better copies…"));
    Q_EMIT betterQualityRequested(tracks);
}

void CollectionView::showSearched(int searching, int already, int best, int gone)
{
    QStringList parts;
    if (searching > 0) {
        parts.append(searching == 1 ? tr("Looking for a better copy of 1 track in Get tracks; the Inbox marks what "
                                         "comes as an upgrade.")
                                    : tr("Looking for better copies of %1 tracks in Get tracks; the Inbox marks what "
                                         "comes as an upgrade.")
                                          .arg(searching));
    }
    if (already > 0) {
        parts.append(already == 1 ? tr("1 is looked for already.") : tr("%1 are looked for already.").arg(already));
    }
    if (best > 0) {
        parts.append(best == 1 ? tr("1 is from lossless already.") : tr("%1 are from lossless already.").arg(best));
    }
    if (gone > 0) {
        parts.append(gone == 1 ? tr("1 is no longer in your collection.")
                               : tr("%1 are no longer in your collection.").arg(gone));
    }
    m_result->setText(parts.join(u' '));
}

void CollectionView::showWallMenu(QPoint position)
{
    const QModelIndex index = m_wall->indexAt(position);
    if (!index.isValid() || (index.flags() & Qt::ItemIsSelectable) == 0) {
        return;
    }
    // As in the Finder: a tile not selected becomes the selection.
    if (!m_wall->selectionModel()->isSelected(index)) {
        m_wall->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
    }
    m_wall->setCurrentIndex(index);
    const quint64 album = index.data(CollectionWallModel::AlbumIdRole).toULongLong();
    const QString path = album != 0 ? index.data(CollectionWallModel::CoverFolderRole).toString()
                                    : index.data(CollectionWallModel::CoverFileRole).toString();
    QMenu menu(this);
    if (album != 0) {
        menu.addAction(tr("Open Album"), this, [this, album] { openAlbum(album); });
    }
    menu.addAction(tr("Show in Finder"), this,
                   [path] { QProcess::startDetached(u"/usr/bin/open"_s, {u"-R"_s, path}); });
    menu.addSeparator();
    QAction *better = menu.addAction(tr("Search for Better Quality"), this,
                                     [this] { searchForBetterQuality(selectedTracks()); });
    better->setEnabled(!upgradable(selectedTracks()).isEmpty());
    menu.exec(m_wall->viewport()->mapToGlobal(position));
}

} // namespace selecta::app
