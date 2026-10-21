// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/BrowseView.h"

#include "app/Accessibility.h"
#include "app/Format.h"
#include "app/PeerMenu.h"
#include "app/models/BrowseTreeModel.h"
#include "core/EngineHost.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

// Up to this many files, every folder opens when the list arrives; a bigger tree opens its top folders only.
constexpr int kOpenAllFiles = 2000;

} // namespace

BrowseView::BrowseView(core::EngineHost &engine, AppSettings &settings, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_settings(settings)
    , m_model(new BrowseTreeModel(this))
    , m_headline(new QLabel(this))
    , m_details(new QLabel(this))
    , m_refresh(new QPushButton(tr("Refresh"), this))
    , m_download(new QPushButton(tr("Download"), this))
    , m_downloadFolder(new QPushButton(tr("Download Folder as Album…"), this))
    , m_pages(new QStackedWidget(this))
    , m_waiting(new QLabel(this))
    , m_table(new QTreeView(this))
{
    setObjectName(u"view.browse"_s);
    QFont headlineFont = m_headline->font();
    headlineFont.setWeight(QFont::DemiBold);
    headlineFont.setPointSizeF(headlineFont.pointSizeF() + 2.0);
    m_headline->setFont(headlineFont);
    m_headline->setObjectName(u"browse.headline"_s);
    m_headline->setTextFormat(Qt::PlainText); // the peer's name
    m_details->setObjectName(u"browse.details"_s);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    m_details->setWordWrap(true);
    m_refresh->setObjectName(u"browse.refresh"_s);
    m_refresh->setToolTip(tr("Ask the peer for the list again (it is kept a day; the governor paces browses)"));
    m_download->setObjectName(u"browse.download"_s);
    m_downloadFolder->setObjectName(u"browse.downloadFolder"_s);
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_engine.browse(m_peer, true); });
    connect(m_download, &QPushButton::clicked, this, &BrowseView::downloadSelected);
    connect(m_downloadFolder, &QPushButton::clicked, this, &BrowseView::downloadFolder);

    // Folders as the peer has them; the search filters only dim what they would hide.
    m_model->setFilter(m_settings.searchViewState().filter);
    m_table->setObjectName(u"browse.results"_s);
    m_table->setModel(m_model);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(BrowseTreeModel::NameColumn, Qt::AscendingOrder); // a folder plays in track order
    QHeaderView *header = m_table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(BrowseTreeModel::NameColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(BrowseTreeModel::StatusColumn, 130);
    m_table->setColumnWidth(BrowseTreeModel::QualityColumn, 110);
    m_table->setColumnWidth(BrowseTreeModel::LengthColumn, 70);
    m_table->setColumnWidth(BrowseTreeModel::SizeColumn, 80);
    m_table->installEventFilter(this); // Return downloads, Space previews
    connect(m_table, &QTreeView::customContextMenuRequested, this, &BrowseView::contextMenu);
    m_table->setAccessibleName(tr("Shared files"));
    a11y::addMenuKey(m_table);
    connect(m_table, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (m_model->resultAt(index)) {
            Q_EMIT previewRequested();
        }
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        updateButtons();
        Q_EMIT selectionMoved();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, &BrowseView::openFolders);

    m_waiting->setObjectName(u"browse.waiting"_s);
    m_waiting->setTextFormat(Qt::PlainText); // the peer's name, or why the list didn't come
    m_waiting->setAlignment(Qt::AlignCenter);
    m_waiting->setWordWrap(true);
    m_waiting->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(m_waiting);
    m_pages->addWidget(m_table);

    auto *top = new QHBoxLayout;
    top->addWidget(m_headline, 1);
    top->addWidget(m_refresh);
    top->addWidget(m_downloadFolder);
    top->addWidget(m_download);
    auto *bar = new QVBoxLayout;
    bar->setContentsMargins(16, 10, 16, 8);
    bar->setSpacing(4);
    bar->addLayout(top);
    bar->addWidget(m_details);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_pages, 1);

    connect(&m_engine, &core::EngineHost::browseChanged, this, [this](const core::Browse &browse) {
        if (browse.peer != m_peer) {
            return;
        }
        // No fresh list is coming (it failed, or the governor held the request back): the one shown is the answer.
        const bool settled = browse.state == core::Browse::State::Failed ||
                             (browse.state == core::Browse::State::Ready && !browse.message.isEmpty());
        if (settled) {
            revealPending(true);
        }
        refresh();
    });
    connect(&m_engine, &core::EngineHost::browseResultsReset, this,
            [this](const QString &peer, const QList<core::SearchResult> &results) {
                if (peer == m_peer) {
                    // The file picked in the list shown so far stays picked in the fresh one.
                    const QByteArray kept = currentPath();
                    m_model->reset(results);
                    if (!m_revealPath.isEmpty()) {
                        revealPending(true);
                    } else if (!kept.isEmpty()) {
                        select(m_model->indexOfPath(kept));
                    }
                    refresh();
                }
            });
    refresh();
}

void BrowseView::showPeer(const QString &peer)
{
    // Shown again (from the sidebar, or a menu): the file picked stays picked.
    const QByteArray kept = peer == m_peer ? currentPath() : QByteArray();
    m_peer = peer;
    m_missing.clear();
    m_table->selectionModel()->clear();
    m_model->setFilter(m_settings.searchViewState().filter); // the filters may have changed in a search
    m_model->reset(m_engine.browseResults(peer));
    if (!m_revealPath.isEmpty()) {
        revealPending(false); // the list shown may be an older one: the fresh one may still have the file
    } else if (!kept.isEmpty()) {
        select(m_model->indexOfPath(kept));
    }
    refresh();
}

QByteArray BrowseView::currentPath() const
{
    const core::SearchResult *current = m_model->resultAt(m_table->currentIndex());
    return current != nullptr ? current->hit.path : QByteArray();
}

void BrowseView::revealFile(const QString &peer, const QByteArray &remotePath)
{
    m_revealPeer = peer;
    m_revealPath = remotePath;
}

void BrowseView::revealPending(bool final)
{
    if (m_revealPath.isEmpty() || m_revealPeer != m_peer) {
        return;
    }
    const QModelIndex found = m_model->indexOfPath(m_revealPath);
    if (!found.isValid()) {
        if (final) {
            m_missing = core::remoteNameOf(core::decodeRemotePath(m_revealPath));
            m_revealPath.clear();
        }
        return;
    }
    select(found);
    // Kept until a fresh list comes: the one shown now may be older, and the fresh one replaces its rows.
    if (final) {
        m_revealPath.clear();
    }
    m_missing.clear();
}

void BrowseView::select(const QModelIndex &file)
{
    if (!file.isValid()) {
        return;
    }
    for (QModelIndex folder = file.parent(); folder.isValid(); folder = folder.parent()) {
        m_table->expand(folder);
    }
    m_table->selectionModel()->setCurrentIndex(file, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_table->scrollTo(file, QAbstractItemView::PositionAtCenter);
}

void BrowseView::openFolders()
{
    if (m_model->fileCount() <= kOpenAllFiles) {
        m_table->expandAll();
        return;
    }
    // A big tree: its top folders, and down every chain of lone folders ("@@music" › "Music" › …).
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QModelIndex folder = m_model->index(row, 0);
        while (folder.isValid() && folder.data(BrowseTreeModel::IsFolderRole).toBool()) {
            m_table->expand(folder);
            folder = m_model->rowCount(folder) == 1 ? m_model->index(0, 0, folder) : QModelIndex();
        }
    }
}

QString BrowseView::title() const
{
    return m_peer.isEmpty() ? tr("Browse") : tr("%1's shares").arg(m_peer);
}

QString BrowseView::summary() const
{
    const std::optional<core::Browse> browse = m_engine.browseOf(m_peer);
    if (!browse) {
        return {};
    }
    switch (browse->state) {
    case core::Browse::State::Requesting:
        return tr("Asking for the list…");
    case core::Browse::State::Receiving:
        return browse->total > 0 ? tr("Receiving the list · %1 of %2")
                                       .arg(format::size(browse->received), format::size(browse->total))
                                 : tr("Receiving the list…");
    case core::Browse::State::Failed:
        return browse->message;
    case core::Browse::State::Ready:
        break;
    }
    const core::FilterCounts counts = m_model->counts();
    if (counts.total == 0 || counts.shown == counts.total) {
        return browse->files == 1 ? tr("1 file") : tr("%1 files").arg(QLocale().toString(browse->files));
    }
    // Every file is listed; those the search filters would hide are dimmed.
    return tr("%1 files · %2 pass your search filters")
        .arg(QLocale().toString(browse->files), QLocale().toString(counts.shown));
}

void BrowseView::refresh()
{
    const std::optional<core::Browse> browse = m_engine.browseOf(m_peer);
    m_headline->setText(title());
    const bool ready = browse && browse->state == core::Browse::State::Ready;
    if (!browse) {
        m_waiting->setText(tr("Nothing to show."));
    } else if (!ready) {
        m_waiting->setText(summary());
    }
    m_pages->setCurrentIndex(ready && m_model->rowCount() > 0 ? 1 : 0);
    if (ready && m_model->rowCount() == 0) {
        m_waiting->setText(browse->files == 0 ? tr("%1 shares nothing.").arg(m_peer) : tr("Reading the list…"));
    }
    QStringList details;
    if (ready && browse->files > 0) {
        details.append(browse->folders == 1
                           ? tr("%1 files in 1 folder").arg(QLocale().toString(browse->files))
                           : tr("%1 files in %2 folders")
                                 .arg(QLocale().toString(browse->files), QLocale().toString(browse->folders)));
        if (browse->privateFolders > 0) {
            details.append(tr("%1 only for their buddies").arg(browse->privateFolders));
        }
        details.append(
            browse->fromCache
                ? tr("as it was %1").arg(format::ago(browse->fetchedAtMs, QDateTime::currentMSecsSinceEpoch()))
                : tr("just listed"));
    }
    if (ready && !browse->message.isEmpty()) {
        details.append(browse->message); // a refresh the governor held back
    }
    if (ready && !m_missing.isEmpty()) {
        details.append(tr("%1 isn't in this list").arg(m_missing));
    }
    m_details->setText(details.join(u" · "_s));
    m_refresh->setEnabled(
        browse && (browse->state == core::Browse::State::Ready || browse->state == core::Browse::State::Failed));
    updateButtons();
    Q_EMIT summaryChanged();
}

void BrowseView::updateButtons()
{
    const std::optional<core::Browse> browse = m_engine.browseOf(m_peer);
    const bool ready = browse && browse->state == core::Browse::State::Ready;
    m_download->setEnabled(ready);
    // A folder with only subfolders has no album of its own.
    m_downloadFolder->setEnabled(ready && folderAnchor().has_value());
    m_downloadFolder->setToolTip(ready && !folderAnchor() ? tr("Pick a folder that holds files, or one of its files")
                                                          : QString());
}

QList<int> BrowseView::selectedResults() const
{
    QList<int> results;
    QSet<int> seen;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        const core::SearchResult *picked = m_model->resultAt(row);
        // A file picked by name goes, filters or not; a folder stands for what passes them.
        const QList<int> indexes = picked != nullptr ? QList<int>{picked->index} : m_model->filesIn(row);
        for (const int index : indexes) {
            const core::SearchResult *result = m_model->result(index);
            const bool wanted = picked != nullptr || m_model->hiddenReason(index) == core::HiddenReason::None;
            if (result && result->isAudio() && wanted && !seen.contains(index)) {
                seen.insert(index);
                results.append(index);
            }
        }
    }
    return results;
}

std::optional<int> BrowseView::folderAnchor() const
{
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid()) {
        return std::nullopt;
    }
    if (const core::SearchResult *result = m_model->resultAt(current)) {
        return result->index;
    }
    const QList<int> under = m_model->filesIn(current.siblingAtColumn(0));
    return under.isEmpty() ? std::nullopt : std::optional(under.constFirst());
}

std::optional<int> BrowseView::previewTarget() const
{
    const core::SearchResult *result = m_model->resultAt(m_table->currentIndex());
    return result && result->isAudio() ? std::optional(result->index) : std::nullopt;
}

void BrowseView::downloadSelected()
{
    const QList<int> selected = selectedResults();
    if (!selected.isEmpty()) {
        m_engine.downloadBrowsed(m_peer, selected);
    }
}

void BrowseView::downloadFolder()
{
    if (const std::optional<int> anchor = folderAnchor()) {
        m_engine.downloadBrowsedFolder(m_peer, *anchor);
    }
}

void BrowseView::contextMenu(QPoint position)
{
    const QList<int> selected = selectedResults();
    QMenu menu(this);
    if (!selected.isEmpty()) {
        menu.addAction(tr("Preview"), this, [this] { Q_EMIT previewRequested(); });
        menu.addAction(selected.size() == 1 ? tr("Download") : tr("Download %1 Files").arg(selected.size()), this,
                       &BrowseView::downloadSelected);
    }
    if (folderAnchor()) {
        menu.addAction(tr("Download Folder as Album…"), this, &BrowseView::downloadFolder);
    }
    if (!selected.isEmpty()) {
        menu.addSeparator();
        menu.addAction(tr("Copy Path"), this, [this, selected] {
            QStringList paths;
            for (const int index : selected) {
                if (const core::SearchResult *result = m_model->result(index)) {
                    paths.append(result->path);
                }
            }
            QApplication::clipboard()->setText(paths.join(u'\n'));
        });
    }
    menu.addSeparator();
    PeerMenu::addActions(menu, m_peer, m_engine,
                         {.browse = {}, .message = [this](const QString &peer) { Q_EMIT messageRequested(peer); }});
    menu.exec(m_table->viewport()->mapToGlobal(position));
}

bool BrowseView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table && event->type() == QEvent::KeyPress) {
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        if (key && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::NoModifier) {
            const QModelIndex current = m_table->currentIndex();
            if (current.isValid() && current.data(BrowseTreeModel::IsFolderRole).toBool()) {
                downloadFolder();
            } else {
                downloadSelected();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace selecta::app
