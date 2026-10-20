// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/CollectionView.h"

#include "app/Format.h"
#include "app/Icons.h"
#include "app/views/EmptyStateView.h"
#include "core/EngineHost.h"
#include "library/HistoryStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

CollectionView::CollectionView(core::EngineHost &engine, storage::Database &db, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_db(db)
    , m_pages(new QStackedWidget(this))
    , m_headline(new QLabel(this))
    , m_details(new QLabel(this))
    , m_message(new QLabel(this))
    , m_progress(new QProgressBar(this))
    , m_scan(new QPushButton(tr("Scan Now"), this))
    , m_unreadableTitle(new QLabel(this))
    , m_unreadable(new QListWidget(this))
    , m_flagsTitle(new QLabel(this))
    , m_flags(new QListWidget(this))
    , m_formats(new QLabel(this))
    , m_inbox(new QLabel(this))
    , m_openInbox(new QPushButton(tr("Open Inbox"), this))
    , m_filedTitle(new QLabel(tr("Recently filed"), this))
    , m_filed(new QListWidget(this))
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

    // The index.
    auto *page = new QWidget(m_pages);
    m_headline->setObjectName(u"collection.headline"_s);
    QFont headlineFont = m_headline->font();
    headlineFont.setPointSizeF(headlineFont.pointSizeF() + 3.0);
    headlineFont.setWeight(QFont::DemiBold);
    m_headline->setFont(headlineFont);
    m_details->setObjectName(u"collection.details"_s);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setObjectName(u"collection.message"_s);
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
    folders->setObjectName(u"collection.folders"_s);
    connect(folders, &QPushButton::clicked, this, &CollectionView::settingsRequested);
    m_unreadableTitle->setObjectName(u"collection.unreadableTitle"_s);
    m_unreadable->setObjectName(u"collection.unreadable"_s);
    m_unreadable->setAccessibleName(tr("Files that couldn't be read"));
    m_unreadable->setSelectionMode(QAbstractItemView::NoSelection);
    m_unreadable->setFrameShape(QFrame::NoFrame);
    m_formats->setObjectName(u"collection.formats"_s);
    m_formats->setWordWrap(true);
    m_inbox->setObjectName(u"collection.inbox"_s);
    m_openInbox->setObjectName(u"collection.openInbox"_s);
    connect(m_openInbox, &QPushButton::clicked, this, &CollectionView::inboxRequested);
    m_filedTitle->setObjectName(u"collection.filedTitle"_s);
    QFont sectionFont = m_filedTitle->font();
    sectionFont.setWeight(QFont::DemiBold);
    m_filedTitle->setFont(sectionFont);
    m_filed->setObjectName(u"collection.filed"_s);
    m_filed->setAccessibleName(tr("Recently filed"));
    m_filed->setFrameShape(QFrame::NoFrame);
    m_filed->setSelectionMode(QAbstractItemView::NoSelection);
    m_filed->setMaximumHeight(200);
    m_flagsTitle->setObjectName(u"collection.flagsTitle"_s);
    m_flags->setObjectName(u"collection.flags"_s);
    m_flags->setAccessibleName(tr("Singles also on an album"));
    m_flags->setFrameShape(QFrame::NoFrame);
    m_flags->setContextMenuPolicy(Qt::CustomContextMenu);
    m_flags->setMaximumHeight(180);
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

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_headline, 1);
    buttons->addWidget(folders);
    buttons->addWidget(m_scan);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(8);
    layout->addLayout(buttons);
    layout->addWidget(m_details);
    layout->addWidget(m_formats);
    layout->addWidget(m_progress);
    layout->addWidget(m_message);
    layout->addSpacing(8);
    auto *inboxRow = new QHBoxLayout;
    inboxRow->addWidget(m_inbox, 1);
    inboxRow->addWidget(m_openInbox);
    layout->addLayout(inboxRow);
    layout->addSpacing(8);
    layout->addWidget(m_filedTitle);
    layout->addWidget(m_filed);
    layout->addSpacing(8);
    layout->addWidget(m_flagsTitle);
    layout->addWidget(m_flags);
    layout->addWidget(m_unreadableTitle);
    layout->addWidget(m_unreadable, 1);
    layout->addStretch(0);
    m_pages->addWidget(page);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);

    connect(&m_engine, &core::EngineHost::collectionStatusChanged, this, &CollectionView::show);
    connect(&m_engine, &core::EngineHost::supersedeFlagsChanged, this, &CollectionView::showFlags);
    // Filing moves Inbox items into the collection: both lists change together.
    connect(&m_engine, &core::EngineHost::inboxChanged, this, &CollectionView::showInboxAndFiled);
    show(m_engine.collectionStatus());
    showFlags(m_engine.supersedeFlags());
    showInboxAndFiled();
}

void CollectionView::showInboxAndFiled()
{
    const QList<core::InboxEntry> entries = m_engine.inboxEntries();
    if (entries.isEmpty()) {
        m_inbox->setText(tr("Nothing waiting in the Inbox."));
    } else {
        m_inbox->setText(entries.size() == 1 ? tr("1 item waiting in the Inbox to be filed.")
                                             : tr("%1 items waiting in the Inbox to be filed.").arg(entries.size()));
    }
    m_openInbox->setVisible(!entries.isEmpty());
    m_filed->clear();
    const QList<library::HistoryEntry> filed = library::HistoryStore(m_db).recentlyFiled(8);
    for (const library::HistoryEntry &entry : filed) {
        const QString name = entry.artist.isEmpty() ? entry.title : entry.artist + u" – "_s + entry.title;
        auto *item = new QListWidgetItem(name.isEmpty() ? QFileInfo(entry.filedPath).fileName() : name, m_filed);
        item->setToolTip(QDir::toNativeSeparators(entry.filedPath));
    }
    m_filedTitle->setVisible(!filed.isEmpty());
    m_filed->setVisible(!filed.isEmpty());
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
        item->setToolTip(QDir::toNativeSeparators(flag.singlePath) + u'\n' + flag.summary);
    }
    const int count = m_flags->count();
    m_flagsTitle->setText(count == 1 ? tr("1 single in your collection is also on an album. Selecta never changes "
                                          "it: keep both, or remove the single yourself.")
                                     : tr("%1 singles in your collection are also on an album. Selecta never "
                                          "changes them: keep both, or remove the singles yourself.")
                                           .arg(count));
    m_flagsTitle->setWordWrap(true);
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
    return m_status.tracks == 1 ? tr("1 track") : tr("%1 tracks").arg(QLocale().toString(m_status.tracks));
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
    m_headline->setText(status.tracks == 1 ? tr("1 track") : tr("%1 tracks").arg(locale.toString(status.tracks)));

    QStringList details;
    if (status.albums > 0) {
        details.append(status.albums == 1 ? tr("1 album") : tr("%1 albums").arg(locale.toString(status.albums)));
    }
    if (status.bytes > 0) {
        details.append(format::size(status.bytes));
    }
    QStringList folders;
    for (const QString &root : status.roots) {
        folders.append(QDir::toNativeSeparators(root));
    }
    details.append(folders.size() == 1 ? tr("in %1").arg(folders.constFirst())
                                       : tr("in %1 folders: %2").arg(folders.size()).arg(folders.join(u", "_s)));
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

    m_progress->setVisible(status.scanning);
    m_progress->setRange(0, status.total > 0 ? status.total : 0); // 0…0: busy
    m_progress->setValue(status.done);
    m_scan->setEnabled(!status.scanning);
    m_message->setText(status.message);
    m_message->setVisible(!status.message.isEmpty());

    m_unreadable->clear();
    for (const auto &[path, reason] : status.unreadableSample) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(path) + u"\n"_s + reason, m_unreadable);
        item->setToolTip(reason);
    }
    m_unreadableTitle->setText(status.unreadable == 1
                                   ? tr("1 file couldn't be read")
                                   : tr("%1 files couldn't be read").arg(locale.toString(status.unreadable)));
    m_unreadableTitle->setVisible(status.unreadable > 0);
    m_unreadable->setVisible(status.unreadable > 0);
    Q_EMIT summaryChanged();
}

} // namespace selecta::app
