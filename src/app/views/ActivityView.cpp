// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/ActivityView.h"

#include "app/MeterDelegate.h"
#include "app/PeerMenu.h"
#include "app/models/ActivityModel.h"
#include "app/models/PeersModel.h"
#include "app/models/TransfersModel.h"
#include "core/ActivityLog.h"
#include "core/EngineHost.h"
#include "core/SearchResults.h"
#include "governor/Limits.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::app {
namespace {

constexpr auto kFlushPace = 500ms;

QTreeView *table(const QString &name, QWidget *parent)
{
    auto *view = new QTreeView(parent);
    view->setObjectName(name);
    view->setUniformRowHeights(true);
    view->setAlternatingRowColors(true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setFrameShape(QFrame::NoFrame);
    view->setAttribute(Qt::WA_MacShowFocusRect, false);
    return view;
}

// A monotonic clock for transfer speeds, shared by every row.
qint64 monotonicMs()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return clock.elapsed();
}

} // namespace

ActivityView::ActivityView(core::EngineHost &engine, storage::Database &db, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_db(db)
    , m_model(new ActivityModel(this))
    , m_transfers(new TransfersModel(this))
    , m_peers(new PeersModel(this))
    , m_pausedBanner(new QWidget(this))
    , m_pages(new QTabWidget(this))
    , m_budget(new QLabel(this))
    , m_table(table(u"activity.log"_s, this))
    , m_transfersTable(table(u"activity.transfers"_s, this))
    , m_grouping(new QComboBox(this))
    , m_peersTable(table(u"activity.peers"_s, this))
    , m_peersNote(new QLabel(this))
    , m_flushTimer(new QTimer(this))
{
    setObjectName(u"view.activity"_s);

    // The banner says why nothing happens while paused, with the way out next to it.
    m_pausedBanner->setObjectName(u"activity.pausedBanner"_s);
    auto *bannerText = new QLabel(tr("Network activity is paused: no new searches or transfers."), m_pausedBanner);
    bannerText->setWordWrap(true);
    auto *resume = new QPushButton(tr("Resume"), m_pausedBanner);
    resume->setObjectName(u"activity.resume"_s);
    connect(resume, &QPushButton::clicked, this, [this] { m_engine.setPaused(false); });
    auto *bannerLayout = new QHBoxLayout(m_pausedBanner);
    bannerLayout->setContentsMargins(16, 10, 16, 0);
    bannerLayout->addWidget(bannerText, 1);
    bannerLayout->addWidget(resume);

    m_pages->setObjectName(u"activity.pages"_s);
    m_pages->setDocumentMode(true);
    m_pages->addTab(createTransfersPage(), tr("Transfers"));
    m_pages->addTab(createPeersPage(), tr("Peers"));
    m_pages->addTab(createLogPage(), tr("Log"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_pausedBanner);
    layout->addWidget(m_pages, 1);

    // Follow the engine first, then load: a line logged in between arrives twice and the model drops the copy.
    connect(&m_engine, &core::EngineHost::activityRecorded, m_model, &ActivityModel::prepend);
    connect(&m_engine, &core::EngineHost::pausedChanged, this, &ActivityView::refreshState);
    connect(&m_engine, &core::EngineHost::searchBudgetChanged, this, &ActivityView::refreshState);
    connect(&m_engine, &core::EngineHost::downloadsReset, this, [this] {
        QHash<qint64, std::pair<qint64, qint64>> progress;
        const QList<core::Download> downloads = m_engine.downloads();
        for (const core::Download &download : downloads) {
            progress.insert(download.id, m_engine.progressOf(download.id));
        }
        m_transfers->reset(downloads, progress);
        m_transfersTable->expandAll();
        Q_EMIT summaryChanged();
    });
    connect(&m_engine, &core::EngineHost::downloadChanged, this, [this](const core::Download &download) {
        m_transfers->update(download);
        Q_EMIT summaryChanged();
    });
    connect(&m_engine, &core::EngineHost::downloadProgress, this, [this](qint64 id, qint64 done, qint64 total) {
        m_transfers->progress(id, done, total, monotonicMs());
        if (!m_flushTimer->isActive()) {
            m_flushTimer->start();
        }
    });
    connect(&m_engine, &core::EngineHost::uploadsChanged, this, [this](const QList<core::UploadEntry> &uploads) {
        m_transfers->setUploads(uploads);
        m_transfersTable->expandAll();
        Q_EMIT summaryChanged();
    });
    connect(&m_engine, &core::EngineHost::peerStatsChanged, this, &ActivityView::refreshPeers);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(kFlushPace);
    connect(m_flushTimer, &QTimer::timeout, m_transfers, &TransfersModel::flush);

    // What the engine already reported.
    QHash<qint64, std::pair<qint64, qint64>> progress;
    const QList<core::Download> downloads = m_engine.downloads();
    for (const core::Download &download : downloads) {
        progress.insert(download.id, m_engine.progressOf(download.id));
    }
    m_transfers->reset(downloads, progress);
    m_transfers->setUploads(m_engine.uploads());
    m_transfersTable->expandAll();
    reload();
    refreshState();
    refreshPeers();
}

QWidget *ActivityView::createTransfersPage()
{
    auto *page = new QWidget(this);
    m_grouping->setObjectName(u"activity.grouping"_s);
    m_grouping->addItem(tr("By Folder"));
    m_grouping->addItem(tr("By Peer"));
    m_grouping->setAccessibleName(tr("Group transfers"));
    connect(m_grouping, &QComboBox::currentIndexChanged, this, [this](int choice) {
        m_transfers->setGrouping(choice == 1 ? TransfersModel::Grouping::Peer : TransfersModel::Grouping::Folder);
        m_transfersTable->expandAll();
    });
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(16, 10, 16, 6);
    auto *explanation =
        new QLabel(tr("Each file Selecta wants, from asking the peer to the Inbox. Right-click to cancel one or try a "
                      "failed one again."),
                   page);
    explanation->setForegroundRole(QPalette::PlaceholderText);
    explanation->setWordWrap(true);
    bar->addWidget(explanation, 1);
    bar->addWidget(m_grouping);

    m_transfersTable->setModel(m_transfers);
    m_transfersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_transfersTable->setItemDelegateForColumn(TransfersModel::ProgressColumn, new MeterDelegate(m_transfersTable));
    m_transfersTable->setContextMenuPolicy(Qt::CustomContextMenu);
    QHeaderView *header = m_transfersTable->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(TransfersModel::ItemColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(TransfersModel::PeerColumn, QHeaderView::Interactive);
    header->setSectionResizeMode(TransfersModel::StatusColumn, QHeaderView::Interactive);
    header->setSectionResizeMode(TransfersModel::ProgressColumn, QHeaderView::Fixed);
    m_transfersTable->setColumnWidth(TransfersModel::PeerColumn, 130);
    m_transfersTable->setColumnWidth(TransfersModel::StatusColumn, 260);
    m_transfersTable->setColumnWidth(TransfersModel::ProgressColumn, 110);
    connect(m_transfersTable, &QTreeView::customContextMenuRequested, this, &ActivityView::transfersMenu);
    // A new folder or peer opens as it appears.
    connect(m_transfers, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (!parent.isValid()) {
                    for (int row = first; row <= last; ++row) {
                        m_transfersTable->expand(m_transfers->index(row, 0));
                    }
                }
            });

    // Nothing to list: say what would be here.
    auto *empty = new QLabel(tr("Nothing transferring. Files from Get tracks, searches and albums show here while "
                                "they come in, until Selecta quits."),
                             page);
    empty->setObjectName(u"activity.transfersEmpty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    auto *stack = new QStackedWidget(page);
    stack->addWidget(m_transfersTable);
    stack->addWidget(empty);
    const auto showEmpty = [this, stack] { stack->setCurrentIndex(m_transfers->rowCount() == 0 ? 1 : 0); };
    connect(m_transfers, &QAbstractItemModel::modelReset, stack, showEmpty);
    connect(m_transfers, &QAbstractItemModel::rowsInserted, stack, showEmpty);
    connect(m_transfers, &QAbstractItemModel::rowsRemoved, stack, showEmpty);
    showEmpty();

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(stack, 1);
    return page;
}

QWidget *ActivityView::createPeersPage()
{
    auto *page = new QWidget(this);
    m_peersNote->setObjectName(u"activity.peersNote"_s);
    m_peersNote->setForegroundRole(QPalette::PlaceholderText);
    m_peersNote->setWordWrap(true);
    m_peersNote->setText(tr("Per peer, Selecta keeps at most %1 file arriving and %2 waiting in its queue, warns at %3 "
                            "files a day and stops at %4. Spreading downloads over several peers is fairer and faster.")
                             .arg(governor::limits::kPeerActiveDownloads)
                             .arg(governor::limits::kPeerQueuedFiles)
                             .arg(governor::limits::kPeerDailyWarnFiles)
                             .arg(governor::limits::kPeerDailyStopFiles));
    m_peersTable->setModel(m_peers);
    m_peersTable->setRootIsDecorated(false);
    m_peersTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_peersTable, &QTreeView::customContextMenuRequested, this, [this](QPoint position) {
        const QModelIndex index = m_peersTable->indexAt(position);
        if (!index.isValid()) {
            return;
        }
        QMenu menu(this);
        PeerMenu::addActions(menu, m_peers->peerAt(index.row()).peer, m_engine, peerHandlers());
        menu.exec(m_peersTable->viewport()->mapToGlobal(position));
    });
    connect(m_peersTable, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid()) {
            Q_EMIT browseRequested(m_peers->peerAt(index.row()).peer);
        }
    });
    m_peersTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_peersTable->setItemDelegateForColumn(PeersModel::LimitColumn, new MeterDelegate(m_peersTable));
    QHeaderView *header = m_peersTable->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(PeersModel::StatusColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(PeersModel::LimitColumn, QHeaderView::Fixed);
    m_peersTable->setColumnWidth(PeersModel::PeerColumn, 140);
    m_peersTable->setColumnWidth(PeersModel::TodayColumn, 100);
    m_peersTable->setColumnWidth(PeersModel::SessionColumn, 60);
    m_peersTable->setColumnWidth(PeersModel::AllTimeColumn, 110);
    m_peersTable->setColumnWidth(PeersModel::LastColumn, 90);
    m_peersTable->setColumnWidth(PeersModel::LimitColumn, 90);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *noteBox = new QVBoxLayout;
    noteBox->setContentsMargins(16, 10, 16, 6);
    noteBox->addWidget(m_peersNote);
    layout->addLayout(noteBox);
    layout->addWidget(m_peersTable, 1);
    // The counters move with the transfers; "12 min ago" moves with the clock.
    connect(m_pages, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == static_cast<int>(Page::Peers)) {
            m_engine.refreshPeerStats();
        }
    });
    return page;
}

QWidget *ActivityView::createLogPage()
{
    auto *page = new QWidget(this);
    m_budget->setObjectName(u"activity.budget"_s);
    m_budget->setForegroundRole(QPalette::PlaceholderText);
    m_budget->setWordWrap(true);

    m_table->setModel(m_model);
    m_table->setRootIsDecorated(false);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->header()->setStretchLastSection(true);
    m_table->header()->setSectionResizeMode(ActivityModel::Time, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(ActivityModel::Operation, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(ActivityModel::Subject, QHeaderView::Interactive);
    m_table->setColumnWidth(ActivityModel::Subject, 280);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *budgetBox = new QVBoxLayout;
    budgetBox->setContentsMargins(16, 10, 16, 10);
    budgetBox->addWidget(m_budget);
    layout->addLayout(budgetBox);
    layout->addWidget(m_table, 1);
    return page;
}

void ActivityView::showPage(Page page)
{
    m_pages->setCurrentIndex(static_cast<int>(page));
}

QString ActivityView::summary() const
{
    if (m_engine.isPaused()) {
        return tr("Network paused");
    }
    // No translator is loaded, so plurals are spelt out rather than left to %n.
    const TransfersModel::Counts counts = m_transfers->counts();
    if (counts.downloading + counts.waiting + counts.uploading > 0) {
        return tr("%1 downloading · %2 waiting · %3 uploading")
            .arg(counts.downloading)
            .arg(counts.waiting)
            .arg(counts.uploading);
    }
    const int available = m_engine.searchesAvailable();
    return available == 1 ? tr("1 search available") : tr("%1 searches available").arg(available);
}

void ActivityView::reload()
{
    m_model->reset(core::ActivityLog::recent(m_db, ActivityModel::kMaxRows));
}

void ActivityView::refreshState()
{
    m_pausedBanner->setVisible(m_engine.isPaused());
    const auto refill = governor::limits::kSearchRefill.count();
    m_budget->setText(tr("Searches you can run now: %1 of %2. One more becomes available every %3 s; Get tracks "
                         "always leaves %4 for searches you type. Every request Selecta makes is listed below, "
                         "with the reason when the governor held it back.")
                          .arg(m_engine.searchesAvailable())
                          .arg(governor::limits::kSearchBurst)
                          .arg(refill)
                          .arg(governor::limits::kGetTracksReservedSearches));
    Q_EMIT summaryChanged();
}

void ActivityView::refreshPeers()
{
    m_peers->reset(m_engine.peerStats(), QDateTime::currentMSecsSinceEpoch());
}

PeerMenu::Handlers ActivityView::peerHandlers()
{
    return {.browse = [this](const QString &peer) { Q_EMIT browseRequested(peer); },
            .message = [this](const QString &peer) { Q_EMIT messageRequested(peer); }};
}

void ActivityView::transfersMenu(QPoint position)
{
    QList<qint64> ids;
    const QModelIndexList rows = m_transfersTable->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        const QList<qint64> under = m_transfers->downloadsAt(row);
        for (const qint64 id : under) {
            if (!ids.contains(id)) {
                ids.append(id);
            }
        }
    }
    QList<qint64> cancellable;
    QList<qint64> failed;
    QStringList paths;
    for (const qint64 id : std::as_const(ids)) {
        const std::optional<core::Download> download = m_engine.download(id);
        if (!download) {
            continue;
        }
        if (download->state == core::Download::State::Failed) {
            failed.append(id);
        } else if (!download->isFinished() && download->state != core::Download::State::Converting) {
            cancellable.append(id);
        }
        paths.append(core::decodeRemotePath(download->remotePath));
    }
    if (paths.isEmpty()) {
        return;
    }
    QMenu menu(this);
    QAction *cancel = menu.addAction(cancellable.size() <= 1 ? tr("Cancel Download")
                                                             : tr("Cancel %1 Downloads").arg(cancellable.size()),
                                     this, [this, cancellable] { m_engine.cancelDownloads(cancellable); });
    cancel->setEnabled(!cancellable.isEmpty());
    QAction *retry = menu.addAction(failed.size() <= 1 ? tr("Try Again") : tr("Try %1 Again").arg(failed.size()), this,
                                    [this, failed] { m_engine.retryDownloads(failed); });
    retry->setEnabled(!failed.isEmpty());
    menu.addSeparator();
    if (const std::optional<core::Download> first = m_engine.download(ids.constFirst())) {
        PeerMenu::addActions(menu, first->peer, m_engine, peerHandlers());
        menu.addSeparator();
    }
    menu.addAction(tr("Copy Path"), this, [paths] { QApplication::clipboard()->setText(paths.join(u'\n')); });
    menu.exec(m_transfersTable->viewport()->mapToGlobal(position));
}

} // namespace selecta::app
