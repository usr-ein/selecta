// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/GetTracksView.h"

#include "app/Icons.h"
#include "app/TwoLineDelegate.h"
#include "app/models/GetTracksModel.h"
#include "app/views/AddTracksDialog.h"
#include "app/views/GetTracksInspector.h"
#include "core/EngineHost.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

GetTracksView::GetTracksView(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_model(new GetTracksModel(this))
    , m_inspector(new GetTracksInspector)
    , m_pages(new QStackedWidget(this))
    , m_table(new QTreeView(this))
    , m_summary(new QLabel(this))
    , m_note(new QLabel(this))
    , m_add(new QPushButton(tr("Add Tracks…"), this))
    , m_find(new QPushButton(tr("Find Them"), this))
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
    connect(m_table, &QTreeView::customContextMenuRequested, this, [this](QPoint position) {
        const QList<qint64> ids = selectedIds();
        if (ids.isEmpty()) {
            return;
        }
        QMenu menu(this);
        menu.addAction(tr("Look Up Again"), this, [this, ids] { m_engine.retryTracks(ids); });
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
    });
    m_table->installEventFilter(this); // ⌫ and ⌦ remove the selected lines

    // Bars.
    m_add->setObjectName(u"get.add"_s);
    connect(m_add, &QPushButton::clicked, this, &GetTracksView::addTracks);
    m_summary->setObjectName(u"get.summary"_s);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    m_note->setObjectName(u"get.note"_s);
    m_note->setWordWrap(true);
    m_note->hide();
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
    bar->setContentsMargins(16, 10, 16, 10);
    bar->addWidget(m_add);
    bar->addSpacing(8);
    bar->addWidget(m_summary, 1);
    bar->addWidget(m_progress);
    bar->addWidget(m_find);
    auto *noteBox = new QHBoxLayout;
    noteBox->setContentsMargins(16, 0, 16, 8);
    noteBox->addWidget(m_note);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
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
    if (counts.lookingUp == 0 && counts.waiting == 0 && counts.ready == 0 && counts.check == 0) {
        parts.append(counts.released == 1 ? tr("1 release found") : tr("%1 releases found").arg(counts.released));
    }
    return parts.join(u" · "_s);
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
    refreshProgress();
    Q_EMIT summaryChanged();
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
    m_find->setVisible(m_model->rowCount() > 0);
    m_find->setText(finding > 0 ? tr("Stop") : tr("Find Them"));
    m_find->setEnabled(finding > 0 || toFind > 0);
    m_find->setDefault(finding == 0 && toFind > 0);

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
    } else if (m_collecting == 0 && m_nextAtMs > 0) {
        const qint64 seconds = std::max<qint64>(0, (m_nextAtMs - QDateTime::currentMSecsSinceEpoch() + 999) / 1000);
        text += u" · "_s + tr("next search in %1 s").arg(seconds);
    }
    m_progress->setText(text);
    if (!m_countdown->isActive()) {
        m_countdown->start();
    }
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
