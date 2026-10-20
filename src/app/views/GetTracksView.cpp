// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/GetTracksView.h"

#include "app/Accessibility.h"
#include "app/Format.h"
#include "app/Icons.h"
#include "app/Presets.h"
#include "app/TickDelegate.h"
#include "app/TwoLineDelegate.h"
#include "app/models/GetTracksModel.h"
#include "app/views/AddTracksDialog.h"
#include "app/views/GetTracksInspector.h"
#include "core/EngineHost.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

GetTracksView::GetTracksView(core::EngineHost &engine, Presets &presets, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_presets(presets)
    , m_model(new GetTracksModel(this))
    , m_inspector(new GetTracksInspector)
    , m_pages(new QStackedWidget(this))
    , m_table(new QTreeView(this))
    , m_summary(new QLabel(this))
    , m_note(new QLabel(this))
    , m_add(new QPushButton(tr("Add Tracks…"), this))
    , m_preset(new QComboBox(this))
    , m_find(new QPushButton(tr("Find Them"), this))
    , m_get(new QPushButton(tr("Get"), this))
    , m_export(new QPushButton(tr("Export Not Found…"), this))
    , m_progress(new QLabel(this))
    , m_countdown(new QTimer(this))
{
    setObjectName(u"view.get"_s);

    // Empty state: the one thing to do here.
    auto *empty = new QWidget(m_pages);
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch(2);
    auto *symbol = new QLabel(empty);
    symbol->setAlignment(Qt::AlignCenter);
    symbol->setPixmap(icons::symbol(u"list.bullet.rectangle"_s, u"view-list-details"_s).pixmap(QSize(44, 44)));
    auto *heading = new QLabel(tr("Paste the tracks you want"), empty);
    heading->setAlignment(Qt::AlignCenter);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 4);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    auto *explain = new QLabel(tr("One per line, “Artist - Title”, or a Spotify playlist link. Selecta finds each "
                                  "release, then the best file on Soulseek. You preview, then each track lands in "
                                  "your Inbox as an MP3 320 with proper tags and its cover."),
                               empty);
    explain->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    explain->setWordWrap(true);
    explain->setForegroundRole(QPalette::PlaceholderText);
    auto *emptyAdd = new QPushButton(tr("Add Tracks…"), empty);
    emptyAdd->setObjectName(u"get.emptyAdd"_s);
    emptyAdd->setDefault(true);
    connect(emptyAdd, &QPushButton::clicked, this, &GetTracksView::addTracks);
    // The wrapped text spans a centred column: an alignment flag on its layout item would size it to its unwrapped
    // hint and clip it (see EmptyStateView).
    auto *column = new QWidget(empty);
    column->setMaximumWidth(460);
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(10);
    columnLayout->addWidget(symbol);
    columnLayout->addWidget(heading);
    columnLayout->addWidget(explain);
    columnLayout->addSpacing(8);
    columnLayout->addWidget(emptyAdd, 0, Qt::AlignHCenter);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(column, 100);
    row->addStretch(1);
    emptyLayout->addLayout(row);
    emptyLayout->addStretch(3);
    m_pages->addWidget(empty);

    // The list.
    m_table->setObjectName(u"get.lines"_s);
    m_table->setModel(m_model);
    m_table->setRootIsDecorated(false);
    m_table->setAlternatingRowColors(true);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setItemDelegateForColumn(GetTracksModel::TickColumn, new TickDelegate(m_table));
    m_table->setItemDelegateForColumn(GetTracksModel::TrackColumn,
                                      new TwoLineDelegate(GetTracksModel::SubtitleRole, m_table));
    QHeaderView *header = m_table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(GetTracksModel::TickColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(GetTracksModel::TrackColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(GetTracksModel::StatusColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(GetTracksModel::FileColumn, QHeaderView::Interactive);
    m_table->setColumnWidth(GetTracksModel::TickColumn, 30);
    m_table->setColumnWidth(GetTracksModel::FileColumn, 240);
    auto *listPage = new QWidget(m_pages);
    auto *listLayout = new QVBoxLayout(listPage);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->addWidget(m_table);
    m_pages->addWidget(listPage);

    connect(m_model, &GetTracksModel::tickRequested, this,
            [this](qint64 id, bool ticked) { m_engine.tickTracks({id}, ticked); });
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &GetTracksView::showSelection);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &GetTracksView::showSelection);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &GetTracksView::selectionMoved);
    connect(m_table, &QTreeView::customContextMenuRequested, this, &GetTracksView::contextMenu);
    m_table->installEventFilter(this); // ⌫ and ⌦ remove the selected lines
    m_table->setAccessibleName(tr("Tracks to get"));
    a11y::addMenuKey(m_table);
    connect(m_table, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid() && index.column() != GetTracksModel::TickColumn) {
            Q_EMIT previewRequested();
        }
    });

    // Bars.
    m_add->setObjectName(u"get.add"_s);
    connect(m_add, &QPushButton::clicked, this, &GetTracksView::addTracks);
    m_summary->setObjectName(u"get.summary"_s);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    m_note->setObjectName(u"get.note"_s);
    m_note->setWordWrap(true);
    m_note->hide();
    // The quality wanted: a preset, as in Search and the Wishlist (BATCH-2, SRCH-8).
    m_preset->setObjectName(u"get.preset"_s);
    m_preset->setAccessibleName(tr("Quality wanted"));
    m_preset->setToolTip(tr("The quality wanted. Changing it ranks the tracks already found again."));
    connect(m_preset, &QComboBox::activated, this, [this](int row) {
        const QString id = m_preset->itemData(row).toString();
        m_presets.setGetTracksId(id);
        m_engine.setGetTracksFilter(m_presets.preset(id).filter);
    });
    connect(&m_presets, &Presets::changed, this, &GetTracksView::rebuildPresets);
    rebuildPresets();
    m_find->setObjectName(u"get.find"_s);
    m_find->setToolTip(tr("Search Soulseek for each track, paced to stay a good citizen of the network: up to 6 "
                          "searches at once, then one every 30 seconds"));
    connect(m_find, &QPushButton::clicked, this, [this] {
        bool finding = false;
        for (const core::GetLine &line : m_model->lines()) {
            finding = finding || line.findState == core::GetLine::FindState::Queued ||
                      line.findState == core::GetLine::FindState::Searching;
        }
        if (finding) {
            m_engine.stopFindingTracks();
        } else {
            m_engine.findTracks();
        }
    });
    m_get->setObjectName(u"get.get"_s);
    m_get->setToolTip(tr("Download the ticked tracks. Each one lands in your Inbox as an MP3 320 with its tags and "
                         "cover."));
    connect(m_get, &QPushButton::clicked, this, [this] { m_engine.downloadTracks(); });
    m_export->setObjectName(u"get.export"_s);
    m_export->setToolTip(tr("Save the lines nothing was found for as a text file, to look for them elsewhere"));
    connect(m_export, &QPushButton::clicked, this, &GetTracksView::exportUnresolved);
    m_progress->setObjectName(u"get.progress"_s);
    m_progress->setForegroundRole(QPalette::PlaceholderText);
    m_countdown->setInterval(1000);
    connect(m_countdown, &QTimer::timeout, this, &GetTracksView::refreshProgress);
    connect(&m_engine, &core::EngineHost::searchQueueChanged, this, [this](int waiting, int collecting, qint64 nextIn) {
        m_waiting = waiting;
        m_collecting = collecting;
        m_nextAtMs = nextIn > 0 ? QDateTime::currentMSecsSinceEpoch() + nextIn : 0;
        refreshProgress();
    });
    connect(&m_engine, &core::EngineHost::networkStatusChanged, this, &GetTracksView::refreshProgress);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(16, 10, 16, 6);
    bar->addWidget(m_add);
    bar->addStretch(1);
    bar->addWidget(m_export);
    bar->addWidget(m_preset);
    bar->addWidget(m_find);
    bar->addWidget(m_get);
    QWidget::setTabOrder({m_add, m_export, m_preset, m_find, m_get, m_table}); // the bar, left to right, then the list
    // The counts and the pace on a line of their own, as in the mock: beside the buttons they ran into each other.
    m_summary->setWordWrap(true);
    m_progress->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *infoBox = new QHBoxLayout;
    infoBox->setContentsMargins(16, 0, 16, 6);
    infoBox->addWidget(m_summary, 1);
    infoBox->addWidget(m_progress);
    auto *noteBox = new QHBoxLayout;
    noteBox->setContentsMargins(16, 0, 16, 8);
    noteBox->addWidget(m_note);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addLayout(infoBox);
    layout->addLayout(noteBox);
    layout->addWidget(m_pages, 1);

    connect(&m_engine, &core::EngineHost::getLinesReset, this, [this](const QList<core::GetLine> &lines) {
        const QList<qint64> keep = selectedIds();
        m_model->reset(lines);
        for (const qint64 id : keep) {
            const int row = m_model->rowOf(id);
            if (row >= 0) {
                m_table->selectionModel()->select(m_model->index(row, 0),
                                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
        }
        refresh();
    });
    connect(&m_engine, &core::EngineHost::getLineChanged, this, [this](const core::GetLine &line) {
        m_model->update(line);
        if (m_inspector->lineId() == line.id) {
            m_inspector->showLine(line);
        }
        refresh();
    });
    connect(&m_engine, &core::EngineHost::tracksAdded, this, &GetTracksView::showAdded);
    // The inspector's ways out (P-004): get a near miss anyway, leave a queue for another peer, find again.
    connect(
        m_inspector, &GetTracksInspector::getAnywayRequested, this,
        [this](qint64 id, const QString &peer, const QByteArray &path) { m_engine.getTrackAnyway(id, peer, path); });
    connect(
        m_inspector, &GetTracksInspector::switchRequested, this,
        [this](qint64 id, const QString &peer, const QByteArray &path) { m_engine.switchTrackPeer(id, peer, path); });
    connect(m_inspector, &GetTracksInspector::findAgainRequested, this,
            [this](qint64 id) { m_engine.findTracks({id}); });
    m_model->reset(m_engine.getLines());
    refresh();
}

QString GetTracksView::summary() const
{
    const GetTracksModel::Counts counts = m_model->counts();
    if (counts.total == 0) {
        return tr("Nothing in the list");
    }
    QStringList parts{counts.total == 1 ? tr("1 track") : tr("%1 tracks").arg(counts.total)};
    if (counts.downloading > 0) {
        parts.append(tr("%1 downloading").arg(counts.downloading));
    }
    if (counts.arrived > 0) {
        parts.append(tr("%1 in your Inbox").arg(counts.arrived));
    }
    if (counts.failed > 0) {
        parts.append(counts.failed == 1 ? tr("1 download failed") : tr("%1 downloads failed").arg(counts.failed));
    }
    if (counts.ready > 0) {
        parts.append(tr("%1 ready").arg(counts.ready));
    }
    if (counts.check > 0) {
        parts.append(tr("%1 to check").arg(counts.check));
    }
    if (counts.notFound > 0) {
        parts.append(tr("%1 not found").arg(counts.notFound));
    }
    if (counts.have > 0) {
        parts.append(tr("%1 you have").arg(counts.have));
    }
    if (counts.lookingUp > 0) {
        parts.append(tr("%1 looking up").arg(counts.lookingUp));
    }
    if (counts.waiting > 0) {
        parts.append(tr("%1 waiting for a release lookup").arg(counts.waiting));
    }
    if (counts.lookingUp == 0 && counts.waiting == 0 && counts.ready == 0 && counts.check == 0 &&
        counts.downloading == 0 && counts.arrived == 0 && counts.failed == 0) {
        parts.append(counts.released == 1 ? tr("1 release found") : tr("%1 releases found").arg(counts.released));
    }
    return parts.join(u" · "_s);
}

std::optional<GetTracksView::PreviewTarget> GetTracksView::previewTarget() const
{
    const QModelIndex current = m_table->currentIndex();
    const QList<qint64> selected = selectedIds();
    qint64 id = current.isValid()
                    ? current.siblingAtColumn(GetTracksModel::TrackColumn).data(GetTracksModel::LineIdRole).toLongLong()
                    : 0;
    if (id == 0 || !selected.contains(id)) {
        if (selected.isEmpty()) {
            return std::nullopt;
        }
        id = selected.constFirst();
    }
    const int row = m_model->rowOf(id);
    if (row < 0) {
        return std::nullopt;
    }
    const core::GetLine &line = m_model->lines().at(row);
    PreviewTarget target;
    target.lineId = id;
    if (line.best) {
        target.peer = line.best->peer;
        target.identity = u"peer|%1|%2|%3"_s.arg(line.best->peer, QString::fromLatin1(line.best->remotePath.toBase64()))
                              .arg(line.best->size);
    }
    return target;
}

QList<qint64> GetTracksView::selectedIds() const
{
    QList<qint64> ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows(GetTracksModel::TrackColumn);
    for (const QModelIndex &row : rows) {
        ids.append(row.data(GetTracksModel::LineIdRole).toLongLong());
    }
    return ids;
}

bool GetTracksView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table && event->type() == QEvent::KeyPress) {
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        if (key && (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete) &&
            key->modifiers() == Qt::NoModifier) {
            removeSelected();
            return true;
        }
        if (key && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::NoModifier) {
            // PREV-4: Return gets the selection (lines with nothing found are left alone by the engine).
            if (const QList<qint64> ids = selectedIds(); !ids.isEmpty()) {
                m_engine.downloadTracks(ids);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GetTracksView::addTracks()
{
    auto *dialog = new AddTracksDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog] {
        const QString text = dialog->text();
        if (!text.trimmed().isEmpty()) {
            m_engine.addTracks(text);
        }
    });
    dialog->open();
}

void GetTracksView::removeSelected()
{
    const QList<qint64> ids = selectedIds();
    if (!ids.isEmpty()) {
        m_engine.removeTracks(ids);
    }
}

void GetTracksView::refresh()
{
    m_pages->setCurrentIndex(m_model->rowCount() == 0 ? 0 : 1);
    m_add->setVisible(m_model->rowCount() > 0); // the empty page has its own button
    m_summary->setText(m_model->rowCount() > 0 ? summary() : QString());
    m_summary->setVisible(m_model->rowCount() > 0);
    refreshProgress();
    Q_EMIT summaryChanged();
}

void GetTracksView::rebuildPresets()
{
    const QSignalBlocker blocker(m_preset);
    m_preset->clear();
    const QList<core::FilterPreset> presets = m_presets.all();
    for (const core::FilterPreset &preset : presets) {
        m_preset->addItem(preset.name, preset.id);
    }
    const QString id = m_presets.getTracksId();
    m_preset->setCurrentIndex(std::max(m_preset->findData(id), 0));
    m_engine.setGetTracksFilter(m_presets.preset(id).filter); // a deleted preset falls back to the default
}

void GetTracksView::refreshProgress()
{
    int toFind = 0;
    int finding = 0;
    for (const core::GetLine &line : m_model->lines()) {
        using FindState = core::GetLine::FindState;
        if (line.downloadState != core::GetLine::DownloadState::None) {
            continue;
        }
        if (line.findState == FindState::Idle) {
            ++toFind;
        } else if (line.findState == FindState::Queued || line.findState == FindState::Searching) {
            ++finding;
        }
    }
    int toGet = 0;
    for (const core::GetLine &line : m_model->lines()) {
        using DownloadState = core::GetLine::DownloadState;
        if (line.ticked && line.best &&
            (line.downloadState == DownloadState::None || line.downloadState == DownloadState::Failed)) {
            ++toGet;
        }
    }
    m_find->setVisible(m_model->rowCount() > 0);
    m_find->setText(finding > 0 ? tr("Stop") : tr("Find Them"));
    m_find->setEnabled(finding > 0 || toFind > 0);
    m_find->setDefault(finding == 0 && toFind > 0 && toGet == 0);
    m_get->setVisible(m_model->rowCount() > 0);
    m_get->setEnabled(toGet > 0);
    m_get->setText(toGet > 1 ? tr("Get %1").arg(toGet) : tr("Get"));
    m_get->setDefault(toGet > 0);
    m_export->setVisible(finding == 0 && !unresolvedLines().isEmpty());

    m_progress->setVisible(finding > 0);
    if (finding == 0) {
        m_progress->clear();
        m_countdown->stop();
        return;
    }
    QString text = finding == 1 ? tr("Finding · 1 left") : tr("Finding · %1 left").arg(finding);
    if (m_engine.networkStatus().state != core::NetworkStatus::State::Online) {
        text += u" · "_s + tr("waiting for the network");
    } else if (m_engine.isPaused()) {
        text += u" · "_s + tr("network paused");
    } else {
        // The whole list's time left at the governor's pace (BATCH-4), and when the next search goes.
        const qint64 nextIn =
            m_nextAtMs > 0 ? std::max<qint64>(0, m_nextAtMs - QDateTime::currentMSecsSinceEpoch()) : 0;
        if (m_collecting == 0 && nextIn > 0) {
            text += u" · "_s + tr("next search in %1 s").arg((nextIn + 999) / 1000);
        }
        const std::chrono::milliseconds left =
            core::findingTimeLeft(std::max(0, finding - m_collecting), m_collecting, m_engine.searchesAvailable(),
                                  std::chrono::milliseconds(nextIn));
        text += u" · "_s + format::about(left.count());
    }
    m_progress->setText(text);
    if (!m_countdown->isActive()) {
        m_countdown->start();
    }
}

void GetTracksView::contextMenu(QPoint position)
{
    const QList<qint64> ids = selectedIds();
    if (ids.isEmpty()) {
        return;
    }
    QMenu menu(this);
    bool canGet = false;
    bool downloading = false;
    QList<qint64> owned; // "You have it", "In your Inbox" (BATCH-3)
    bool tickable = false;
    bool allTicked = true;
    for (const qint64 id : ids) {
        const int row = m_model->rowOf(id);
        if (row < 0) {
            continue;
        }
        const core::GetLine &line = m_model->lines().at(row);
        if (line.findState == core::GetLine::FindState::Owned || line.findState == core::GetLine::FindState::InInbox) {
            owned.append(id);
        }
        using DownloadState = core::GetLine::DownloadState;
        tickable = tickable || line.best.has_value();
        allTicked = allTicked && (!line.best || line.ticked);
        canGet = canGet || (line.best &&
                            (line.downloadState == DownloadState::None || line.downloadState == DownloadState::Failed));
        downloading = downloading || line.downloadState == DownloadState::Queued ||
                      line.downloadState == DownloadState::Transferring ||
                      line.downloadState == DownloadState::Converting;
    }
    if (canGet) {
        menu.addAction(tr("Get"), this, [this, ids] { m_engine.downloadTracks(ids); });
    }
    if (tickable) { // the tick boxes, from the keyboard too (⌃Return opens this menu)
        menu.addAction(allTicked ? tr("Untick") : tr("Tick"), this,
                       [this, ids, allTicked] { m_engine.tickTracks(ids, !allTicked); });
    }
    if (downloading) {
        menu.addAction(tr("Cancel Download"), this, [this, ids] { m_engine.cancelTrackDownloads(ids); });
    }
    if (!owned.isEmpty()) {
        // LIB-6: the copy found isn't this track; remembered, and the line is found like any other.
        menu.addAction(tr("Not What I Have, Find It"), this, [this, owned] { m_engine.notOwnedFindTracks(owned); });
    }
    if (canGet || downloading || !owned.isEmpty()) {
        menu.addSeparator();
    }
    menu.addAction(tr("Look Up Again"), this, [this, ids] { m_engine.retryTracks(ids); });
    // SRCH-14: watch for lines nothing was found for.
    menu.addAction(tr("Add to Wishlist"), this, [this, ids] {
        for (const qint64 id : ids) {
            const int row = m_model->rowOf(id);
            if (row < 0) {
                continue;
            }
            const core::GetLine &line = m_model->lines().at(row);
            const QString text = line.artist.isEmpty() ? line.raw : line.artist + u' ' + line.title;
            const core::FilterPreset preset = m_presets.preset(m_presets.getTracksId());
            m_engine.addToWishlist(text, preset.name, preset.filter);
        }
    });
    menu.addAction(tr("Copy"), this, [this] {
        QStringList lines;
        const QModelIndexList rows = m_table->selectionModel()->selectedRows(GetTracksModel::TrackColumn);
        for (const QModelIndex &row : rows) {
            lines.append(row.data().toString());
        }
        QApplication::clipboard()->setText(lines.join(u'\n'));
    });
    menu.addSeparator();
    menu.addAction(tr("Remove from List"), this, &GetTracksView::removeSelected);
    menu.exec(m_table->viewport()->mapToGlobal(position));
}

QStringList GetTracksView::unresolvedLines() const
{
    QStringList lines;
    for (const core::GetLine &line : m_model->lines()) {
        using ReleaseState = core::GetLine::ReleaseState;
        const bool notFound = line.findState == core::GetLine::FindState::NotFound;
        const bool unsure =
            !line.best && (line.releaseState == ReleaseState::Ambiguous || line.releaseState == ReleaseState::Failed);
        if (notFound || unsure) {
            lines.append(line.raw.trimmed().isEmpty() ? line.artist + u" - "_s + line.title : line.raw.trimmed());
        }
    }
    return lines;
}

void GetTracksView::exportUnresolved()
{
    const QStringList lines = unresolvedLines();
    if (lines.isEmpty()) {
        return;
    }
    auto *dialog = new QFileDialog(this, tr("Export Tracks Not Found"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setDefaultSuffix(u"txt"_s);
    dialog->setNameFilter(tr("Text files (*.txt)"));
    dialog->selectFile(tr("Not found on Soulseek.txt"));
    dialog->setWindowModality(Qt::WindowModal);
    connect(dialog, &QFileDialog::fileSelected, this, [this, lines](const QString &path) {
        QSaveFile file(path);
        const QByteArray text = (lines.join(u'\n') + u'\n').toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
            m_note->setText(tr("Couldn't save %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
            m_note->show();
            return;
        }
        m_note->setText(lines.size() == 1
                            ? tr("Saved 1 line to %1.").arg(QDir::toNativeSeparators(path))
                            : tr("Saved %1 lines to %2.").arg(lines.size()).arg(QDir::toNativeSeparators(path)));
        m_note->show();
    });
    dialog->open();
}

void GetTracksView::showSelection()
{
    const QModelIndex current = m_table->selectionModel()->currentIndex();
    const QList<qint64> ids = selectedIds();
    const core::GetLine *line = nullptr;
    if (current.isValid() && m_table->selectionModel()->isRowSelected(current.row(), {})) {
        line = m_model->lineAt(current.row());
    } else if (!ids.isEmpty()) {
        line = m_model->lineAt(m_model->rowOf(ids.first()));
    }
    m_inspector->showLine(line ? std::optional<core::GetLine>(*line) : std::nullopt);
}

void GetTracksView::showAdded(int added, int duplicates, int unidentified, const QString &note)
{
    QStringList parts;
    if (added > 0) {
        parts.append(added == 1 ? tr("Added 1 track.") : tr("Added %1 tracks.").arg(added));
    }
    if (duplicates > 0) {
        parts.append(duplicates == 1 ? tr("1 was already in the list.")
                                     : tr("%1 were already in the list.").arg(duplicates));
    }
    if (unidentified > 0) {
        parts.append(unidentified == 1 ? tr("1 unidentified track (“ID”) was left out.")
                                       : tr("%1 unidentified tracks (“ID”) were left out.").arg(unidentified));
    }
    if (!note.isEmpty()) {
        parts.append(note);
    }
    if (parts.isEmpty()) {
        parts.append(tr("Nothing to add: no tracks were found in the text."));
    }
    m_note->setText(parts.join(u' '));
    m_note->show();
}

} // namespace selecta::app
