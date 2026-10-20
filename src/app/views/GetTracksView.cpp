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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
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
    explain->setAlignment(Qt::AlignCenter);
    explain->setWordWrap(true);
    explain->setForegroundRole(QPalette::PlaceholderText);
    explain->setMaximumWidth(460);
    auto *emptyAdd = new QPushButton(tr("Add Tracks…"), empty);
    emptyAdd->setObjectName(u"get.emptyAdd"_s);
    emptyAdd->setDefault(true);
    connect(emptyAdd, &QPushButton::clicked, this, &GetTracksView::addTracks);
    emptyLayout->addWidget(symbol);
    emptyLayout->addWidget(heading);
    emptyLayout->addWidget(explain, 0, Qt::AlignHCenter);
    emptyLayout->addSpacing(8);
    emptyLayout->addWidget(emptyAdd, 0, Qt::AlignHCenter);
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
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(16, 10, 16, 10);
    bar->addWidget(m_add);
    bar->addSpacing(8);
    bar->addWidget(m_summary, 1);
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
    Q_EMIT summaryChanged();
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
