// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/BrowseView.h"

#include "app/Format.h"
#include "app/PeerMenu.h"
#include "app/models/SearchResultsModel.h"
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

BrowseView::BrowseView(core::EngineHost &engine, AppSettings &settings, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_settings(settings)
    , m_model(new SearchResultsModel(this))
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
    m_details->setObjectName(u"browse.details"_s);
    m_details->setForegroundRole(QPalette::PlaceholderText);
    m_details->setWordWrap(true);
    m_refresh->setObjectName(u"browse.refresh"_s);
    m_refresh->setToolTip(tr("Ask the peer for the list again (it is kept a day; the governor paces browses)"));
    m_download->setObjectName(u"browse.download"_s);
    m_downloadFolder->setObjectName(u"browse.downloadFolder"_s);
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_engine.browse(m_peer, true); });
    connect(m_download, &QPushButton::clicked, this, &BrowseView::downloadSelected);
    connect(m_downloadFolder, &QPushButton::clicked, this, &BrowseView::downloadFolder);

    // The same filters as searches; folders, as the peer has them.
    m_model->setFilter(m_settings.searchViewState().filter);
    m_model->setGrouped(true);
    m_model->setReleaseChosen(false);
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
    m_table->sortByColumn(SearchResultsModel::TitleColumn, Qt::AscendingOrder); // a folder plays in track order
    QHeaderView *header = m_table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(SearchResultsModel::TitleColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(SearchResultsModel::StatusColumn, 130);
    m_table->setColumnWidth(SearchResultsModel::QualityColumn, 110);
    m_table->setColumnWidth(SearchResultsModel::LengthColumn, 80);
    m_table->setColumnHidden(SearchResultsModel::PeerColumn, true); // one peer
    m_table->setColumnHidden(SearchResultsModel::AvailabilityColumn, true);
    m_table->installEventFilter(this); // Return downloads, Space previews
    connect(m_table, &QTreeView::customContextMenuRequested, this, &BrowseView::contextMenu);
    connect(m_table, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (m_model->resultAt(index)) {
            Q_EMIT previewRequested();
        }
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &BrowseView::selectionMoved);
    connect(m_model, &QAbstractItemModel::modelReset, m_table, &QTreeView::expandAll);

    m_waiting->setObjectName(u"browse.waiting"_s);
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
        if (browse.peer == m_peer) {
            refresh();
        }
    });
    connect(&m_engine, &core::EngineHost::browseResultsReset, this,
            [this](const QString &peer, const QList<core::SearchResult> &results) {
                if (peer == m_peer) {
                    m_model->reset(results);
                    refresh();
                }
            });
    refresh();
}

void BrowseView::showPeer(const QString &peer)
{
    m_peer = peer;
    m_table->selectionModel()->clear();
    m_model->setFilter(m_settings.searchViewState().filter); // the filters may have changed in a search
    m_model->reset(m_engine.browseResults(peer));
    refresh();
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
    return tr("%1 of %2 files shown").arg(QLocale().toString(counts.shown), QLocale().toString(browse->files));
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
        m_waiting->setText(browse->files == 0 ? tr("%1 shares nothing.").arg(m_peer)
                                              : tr("Nothing here passes your search filters."));
    }
    QStringList details;
    if (browse && browse->files > 0) {
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
    if (browse && browse->state == core::Browse::State::Ready && !browse->message.isEmpty()) {
        details.append(browse->message); // a refresh the governor held back
    }
    m_details->setText(details.join(u" · "_s));
    m_refresh->setEnabled(
        browse && (browse->state == core::Browse::State::Ready || browse->state == core::Browse::State::Failed));
    m_download->setEnabled(ready);
    m_downloadFolder->setEnabled(ready);
    Q_EMIT summaryChanged();
}

QList<int> BrowseView::selectedResults() const
{
    QList<int> results;
    QSet<int> seen;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        const QList<int> indexes =
            m_model->resultAt(row) ? QList<int>{m_model->resultAt(row)->index} : m_model->resultsUnder(row);
        for (const int index : indexes) {
            const core::SearchResult *result = m_model->result(index);
            if (result && result->isAudio() && !seen.contains(index)) {
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
    const QList<int> under = m_model->resultsUnder(current.siblingAtColumn(0));
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
            if (current.isValid() && current.data(SearchResultsModel::IsFolderRole).toBool()) {
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
