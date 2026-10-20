// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/InboxView.h"

#include "app/Icons.h"
#include "app/views/EmptyStateView.h"
#include "core/EngineHost.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kIdRole = Qt::UserRole + 1;
enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
    ItemColumn,
    FormatColumn,
    NameColumn,
    FromColumn,
    StatusColumn,
    ColumnCount
};

using library::InboxItem;

QString itemTitle(const InboxItem &item)
{
    const QString artist = item.artist.isEmpty() ? InboxView::tr("Unknown artist") : item.artist;
    if (item.kind == InboxItem::Kind::Album) {
        QString title = artist + u" — "_s + (item.album.isEmpty() ? item.title : item.album);
        if (item.year) {
            title += u" (%1)"_s.arg(*item.year);
        }
        return title;
    }
    QString title = artist + u" — "_s + item.title;
    if (!item.version.isEmpty() && !item.title.contains(item.version, Qt::CaseInsensitive)) {
        title += u" ("_s + item.version + u')';
    }
    return title;
}

QString formatText(const InboxItem &item)
{
    QString text = item.format;
    if (item.bitrate) {
        text += u' ' + QString::number(*item.bitrate);
    } else if (item.sampleRate && item.bitDepth) {
        text += u" %1/%2"_s.arg(*item.bitDepth).arg(*item.sampleRate / 1000.0, 0, 'g', 3);
    }
    return text.trimmed();
}

QString statusText(const core::InboxEntry &entry)
{
    if (entry.busy) {
        return InboxView::tr("Working…");
    }
    switch (entry.item.state) {
    case InboxItem::State::Processing:
        return InboxView::tr("Converting…");
    case InboxItem::State::Failed:
        return InboxView::tr("Failed");
    case InboxItem::State::Ready:
        return InboxView::tr("Ready");
    }
    return {};
}

QStandardItem *cell(const QString &text, qint64 id, const QString &toolTip = {})
{
    auto *item = new QStandardItem(text);
    item->setData(id, kIdRole);
    item->setEditable(false);
    if (!toolTip.isEmpty()) {
        item->setToolTip(toolTip);
    }
    return item;
}

} // namespace

InboxView::InboxView(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_model(new QStandardItemModel(this))
    , m_pages(new QStackedWidget(this))
    , m_table(new QTreeView(this))
    , m_summary(new QLabel(this))
    , m_result(new QLabel(this))
    , m_file(new QPushButton(tr("File"), this))
    , m_trash(new QPushButton(tr("Move to Trash"), this))
{
    setObjectName(u"view.inbox"_s);
    m_summary->setObjectName(u"inbox.summary"_s);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    m_result->setObjectName(u"inbox.result"_s);
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_result->hide();
    m_file->setObjectName(u"inbox.file"_s);
    m_file->setDefault(true);
    m_trash->setObjectName(u"inbox.trash"_s);
    connect(m_file, &QPushButton::clicked, this, &InboxView::fileSelected);
    connect(m_trash, &QPushButton::clicked, this, &InboxView::trashSelected);

    m_model->setHorizontalHeaderLabels({tr("Item"), tr("Format"), tr("File name"), tr("From"), tr("Status")});
    m_table->setObjectName(u"inbox.items"_s);
    m_table->setModel(m_model);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(ItemColumn, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(FormatColumn, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(NameColumn, QHeaderView::Interactive);
    m_table->header()->setSectionResizeMode(FromColumn, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(StatusColumn, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(NameColumn, 260);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &InboxView::refreshActions);
    connect(m_table, &QTreeView::customContextMenuRequested, this, [this](QPoint position) {
        if (selectedIds().isEmpty()) {
            return;
        }
        QMenu menu(this);
        menu.addAction(tr("Reveal in Finder"), this, &InboxView::revealSelected);
        menu.addSeparator();
        menu.addAction(tr("File"), this, &InboxView::fileSelected)->setEnabled(m_file->isEnabled());
        menu.addAction(tr("Move to Trash"), this, &InboxView::trashSelected);
        menu.exec(m_table->viewport()->mapToGlobal(position));
    });
    connect(m_table, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.data(kIdRole).isValid()) {
            Q_EMIT previewRequested();
        }
    });
    auto *trashShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backspace), m_table);
    trashShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(trashShortcut, &QShortcut::activated, this, &InboxView::trashSelected);

    auto *empty =
        new EmptyStateView(icons::symbol(u"tray.and.arrow.down"_s, u"mail-folder-inbox"_s), tr("Your Inbox is empty"),
                           tr("Downloads land here as tagged MP3 320 files with their cover, until you file "
                              "them into your collection."),
                           {}, m_pages);
    m_pages->addWidget(empty);
    auto *list = new QWidget(m_pages);
    auto *listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->addWidget(m_table);
    m_pages->addWidget(list);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(16, 10, 16, 10);
    bar->addWidget(m_summary, 1);
    bar->addWidget(m_trash);
    bar->addWidget(m_file);
    auto *note = new QLabel(tr("Downloads are converted and tagged as they arrive. File moves an item into your "
                               "collection under the names from Settings; Selecta never changes files once they are "
                               "there."),
                            this);
    note->setObjectName(u"inbox.note"_s);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    auto *resultBox = new QVBoxLayout;
    resultBox->setContentsMargins(16, 6, 16, 10);
    resultBox->setSpacing(4);
    resultBox->addWidget(m_result);
    resultBox->addWidget(note);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_pages, 1);
    layout->addLayout(resultBox);

    connect(&m_engine, &core::EngineHost::inboxChanged, this, &InboxView::rebuild);
    connect(&m_engine, &core::EngineHost::inboxOperationFinished, this, &InboxView::showResult);
    rebuild(m_engine.inboxEntries());
}

void InboxView::setLibraryConfig(const core::LibraryConfig &config)
{
    m_config = config;
    refreshActions();
}

void InboxView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_engine.refreshInbox(); // picks up files deleted in Finder meanwhile
}

QString InboxView::summary() const
{
    if (m_entries.isEmpty()) {
        return tr("Nothing waiting");
    }
    return m_entries.size() == 1 ? tr("1 item waiting") : tr("%1 items waiting").arg(m_entries.size());
}

QList<qint64> InboxView::selectedIds() const
{
    QList<qint64> ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows(ItemColumn);
    for (const QModelIndex &row : rows) {
        const QVariant id = row.data(kIdRole);
        if (id.isValid()) {
            ids.append(id.toLongLong());
        }
    }
    return ids;
}

std::optional<InboxView::PreviewTarget> InboxView::previewTarget() const
{
    QVariant id = m_table->currentIndex().siblingAtColumn(ItemColumn).data(kIdRole);
    const QList<qint64> selected = selectedIds();
    if (!id.isValid() || !selected.contains(id.toLongLong())) {
        if (selected.isEmpty()) {
            return std::nullopt;
        }
        id = selected.constFirst();
    }
    for (const core::InboxEntry &entry : std::as_const(m_entries)) {
        const InboxItem &item = entry.item;
        if (item.id != id.toLongLong()) {
            continue;
        }
        PreviewTarget target;
        target.path = QDir(m_config.inboxRoot).filePath(item.relativePath);
        target.track.title = item.kind == InboxItem::Kind::Album && !item.album.isEmpty() ? item.album : item.title;
        if (item.kind == InboxItem::Kind::Single && !item.version.isEmpty() &&
            !item.title.contains(item.version, Qt::CaseInsensitive)) {
            target.track.title += u" ("_s + item.version + u')';
        }
        target.track.artist = item.artist;
        target.track.source = tr("Your Inbox");
        return target;
    }
    return std::nullopt;
}

void InboxView::rebuild(const QList<core::InboxEntry> &entries)
{
    const QList<qint64> previouslySelected = selectedIds();
    m_entries = entries;
    m_model->removeRows(0, m_model->rowCount());

    const auto addGroup = [this](const QString &name, InboxItem::Kind kind) {
        QList<const core::InboxEntry *> members;
        for (const core::InboxEntry &entry : std::as_const(m_entries)) {
            if (entry.item.kind == kind) {
                members.append(&entry);
            }
        }
        if (members.isEmpty()) {
            return;
        }
        auto *group = new QStandardItem(u"%1  %2"_s.arg(name).arg(members.size()));
        group->setEditable(false);
        group->setSelectable(false);
        QFont bold = group->font();
        bold.setWeight(QFont::DemiBold);
        group->setFont(bold);
        QList<QStandardItem *> groupRow{group};
        for (int column = 1; column < ColumnCount; ++column) {
            auto *filler = new QStandardItem;
            filler->setEditable(false);
            filler->setSelectable(false);
            groupRow.append(filler);
        }
        m_model->appendRow(groupRow);
        for (const core::InboxEntry *entry : std::as_const(members)) {
            const InboxItem &item = entry->item;
            const QString fileName = QFileInfo(item.relativePath).fileName();
            QString itemText = itemTitle(item);
            if (item.kind == InboxItem::Kind::Album && item.trackCount) {
                itemText += tr(" · %1 tracks").arg(*item.trackCount);
            }
            const QString source =
                item.sourceFormat.isEmpty() ? QString() : tr("Downloaded as %1").arg(item.sourceFormat);
            group->appendRow({cell(itemText, item.id), cell(formatText(item), item.id, source),
                              cell(fileName, item.id, item.relativePath), cell(item.peer, item.id),
                              cell(statusText(*entry), item.id, item.error)});
        }
    };
    addGroup(tr("Albums"), InboxItem::Kind::Album);
    addGroup(tr("Singles"), InboxItem::Kind::Single);
    m_table->expandAll();

    // Keep the selection across updates, by id.
    const QSet<qint64> keep(previouslySelected.cbegin(), previouslySelected.cend());
    for (int g = 0; g < m_model->rowCount(); ++g) {
        const QStandardItem *group = m_model->item(g);
        for (int r = 0; r < group->rowCount(); ++r) {
            if (keep.contains(group->child(r)->data(kIdRole).toLongLong())) {
                m_table->selectionModel()->select(group->child(r)->index(),
                                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
        }
    }
    m_pages->setCurrentIndex(m_entries.isEmpty() ? 0 : 1);
    refreshActions();
    Q_EMIT summaryChanged();
}

void InboxView::refreshActions()
{
    const QList<qint64> selected = selectedIds();
    const bool haveCollection = !m_config.collectionRoots.isEmpty();
    m_file->setEnabled(!selected.isEmpty() && haveCollection);
    m_file->setText(selected.size() > 1 ? tr("File %1").arg(selected.size()) : tr("File"));
    m_file->setToolTip(
        haveCollection
            ? tr("Move the selection into %1").arg(QDir::toNativeSeparators(m_config.collectionRoots.first()))
            : tr("Choose your collection folder in Settings → Library first"));
    m_trash->setEnabled(!selected.isEmpty());
    if (selected.isEmpty()) {
        m_summary->setText(summary());
    } else {
        m_summary->setText(selected.size() == 1 ? tr("1 selected") : tr("%1 selected").arg(selected.size()));
    }
}

void InboxView::fileSelected()
{
    const QList<qint64> ids = selectedIds();
    if (!ids.isEmpty() && !m_config.collectionRoots.isEmpty()) {
        m_engine.fileInboxItems(ids);
    }
}

void InboxView::trashSelected()
{
    const QList<qint64> ids = selectedIds();
    if (!ids.isEmpty()) {
        m_engine.trashInboxItems(ids);
    }
}

void InboxView::revealSelected()
{
    const QList<qint64> ids = selectedIds();
    if (ids.isEmpty()) {
        return;
    }
    for (const core::InboxEntry &entry : std::as_const(m_entries)) {
        if (entry.item.id == ids.first()) {
            const QString path = QDir(m_config.inboxRoot).filePath(entry.item.relativePath);
            QProcess::startDetached(u"/usr/bin/open"_s, {u"-R"_s, path});
            return;
        }
    }
}

void InboxView::showResult(qint64 id, bool ok, const QString &message)
{
    Q_UNUSED(id)
    m_result->setText(ok ? message : tr("Couldn't do it: %1").arg(message));
    m_result->setForegroundRole(ok ? QPalette::PlaceholderText : QPalette::WindowText);
    m_result->show();
}

} // namespace selecta::app
