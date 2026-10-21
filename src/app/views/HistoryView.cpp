// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/HistoryView.h"

#include "app/Status.h"
#include "app/Style.h"

#include "app/Format.h"
#include "library/HistoryStore.h"
#include "storage/SessionLog.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kSessionsShown = 200;
constexpr int kDownloadsShown = 5000;

using library::HistoryEntry;

QString formatTime(qint64 msSinceEpoch)
{
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(msSinceEpoch), QLocale::ShortFormat);
}

QString formatDuration(qint64 ms)
{
    const qint64 minutes = std::max<qint64>(0, ms) / 60'000;
    if (minutes < 60) {
        return HistoryView::tr("%1 min").arg(minutes);
    }
    return HistoryView::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString downloadsText(qsizetype count)
{
    if (count == 0) {
        return HistoryView::tr("no downloads");
    }
    return count == 1 ? HistoryView::tr("1 download") : HistoryView::tr("%1 downloads").arg(count);
}

QString itemText(const HistoryEntry &entry)
{
    QString title = entry.title;
    if (!entry.version.isEmpty() && !title.contains(entry.version, Qt::CaseInsensitive)) {
        title += u" ("_s + entry.version + u')';
    }
    if (entry.artist.isEmpty() && title.isEmpty()) {
        // No tags known: the peer's file name says what it was.
        const QString path = QString::fromUtf8(entry.remotePath);
        return path.section(u'\\', -1).section(u'/', -1);
    }
    return entry.artist + u" — "_s + title;
}

QString whereText(const HistoryEntry &entry)
{
    switch (entry.whereabouts) {
    case HistoryEntry::Whereabouts::Inbox:
        return HistoryView::tr("In the Inbox");
    case HistoryEntry::Whereabouts::Filed:
        return HistoryView::tr("In your collection"); // where exactly: the tooltip
    case HistoryEntry::Whereabouts::Trashed:
        return HistoryView::tr("Moved to the Trash");
    case HistoryEntry::Whereabouts::Preview:
        return HistoryView::tr("Preview only");
    }
    return {};
}

// Where the file is now, as a status (P-003): a symbol in its kind's colour, then the words.
status::Status whereSymbol(const HistoryEntry &entry)
{
    using status::Kind;
    switch (entry.whereabouts) {
    case HistoryEntry::Whereabouts::Inbox:
        return {u"tray.full.fill"_s, Kind::Inbox};
    case HistoryEntry::Whereabouts::Filed:
        return {u"checkmark.seal.fill"_s, Kind::Have};
    case HistoryEntry::Whereabouts::Trashed:
        return {u"trash"_s, Kind::Waiting};
    case HistoryEntry::Whereabouts::Preview:
        return {u"clock"_s, Kind::Waiting};
    }
    return {};
}

QString whereToolTip(const HistoryEntry &entry)
{
    if (entry.whereabouts == HistoryEntry::Whereabouts::Filed && !entry.filedPath.isEmpty()) {
        return QDir::toNativeSeparators(entry.filedPath);
    }
    return whereText(entry);
}

QStandardItem *readOnly(const QString &text, const QString &toolTip = {})
{
    auto *item = new QStandardItem(text);
    item->setEditable(false);
    if (!toolTip.isEmpty()) {
        item->setToolTip(format::plainToolTip(toolTip)); // a peer's path, a folder
    }
    return item;
}

} // namespace

HistoryView::HistoryView(storage::Database &db, int currentSession, QWidget *parent)
    : QWidget(parent)
    , m_db(db)
    , m_currentSession(currentSession)
    , m_model(new QStandardItemModel(this))
    , m_table(new QTreeView(this))
    , m_summary(new QLabel(this))
{
    setObjectName(u"view.history"_s);
    m_summary->setObjectName(u"history.summary"_s);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    m_summary->setWordWrap(true);

    m_model->setHorizontalHeaderLabels({tr("Session or download"), tr("Quality"), tr("From"), tr("Now")});
    m_table->setObjectName(u"history.sessions"_s);
    m_table->setAccessibleName(tr("Sessions and their downloads"));
    m_table->setModel(m_model);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->header()->setStretchLastSection(true);
    m_table->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_table->setColumnWidth(0, 420);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    // Fewer columns (P-003): the peer is in the download's tooltip; where it is now, as a status.
    m_table->header()->setSectionHidden(2, true);
    m_table->setItemDelegateForColumn(3, new status::StatusDelegate(m_table));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *summaryBox = new QVBoxLayout;
    summaryBox->setContentsMargins(style::kBarMargins);
    summaryBox->addWidget(m_summary);
    layout->addLayout(summaryBox);
    layout->addWidget(m_table, 1);
    refresh();
}

void HistoryView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
}

void HistoryView::refresh()
{
    // Group the newest downloads by session in one query.
    QHash<qint64, QList<HistoryEntry>> bySession;
    library::HistoryStore history(m_db);
    const QList<HistoryEntry> downloads = history.recent(kDownloadsShown);
    for (const HistoryEntry &entry : downloads) {
        bySession[entry.session.value_or(0)].prepend(entry); // oldest first within a session
    }

    m_model->removeRows(0, m_model->rowCount());
    storage::SessionLog sessions(m_db);
    const QList<storage::SessionRecord> records = sessions.recent(kSessionsShown);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qsizetype total = 0;
    for (const storage::SessionRecord &session : records) {
        const bool isCurrent = session.number == m_currentSession;
        QString length;
        if (session.endedAtMs) {
            length = formatDuration(*session.endedAtMs - session.startedAtMs);
        } else if (isCurrent) {
            length = tr("running for %1").arg(formatDuration(now - session.startedAtMs));
        } else {
            length = tr("didn't quit cleanly");
        }
        const QList<HistoryEntry> entries = bySession.value(session.number);
        total += entries.size();
        const QString title = (isCurrent ? tr("Session %1 (this one)") : tr("Session %1")).arg(session.number);
        auto *row = readOnly(
            u"%1 · %2 · %3 · %4"_s.arg(title, formatTime(session.startedAtMs), length, downloadsText(entries.size())));
        QFont bold = row->font();
        bold.setWeight(QFont::DemiBold);
        row->setFont(bold);
        const QList<QStandardItem *> sessionRow{row, readOnly({}), readOnly({}), readOnly({})};
        for (const HistoryEntry &entry : entries) {
            QStandardItem *where = readOnly(whereText(entry), whereToolTip(entry));
            const status::Status symbol = whereSymbol(entry);
            where->setData(symbol.symbol, status::kSymbolRole);
            where->setData(static_cast<int>(symbol.kind), status::kKindRole);
            const QString from = entry.peer.isEmpty() ? QString() : tr("From %1").arg(entry.peer);
            const QString path = QString::fromUtf8(entry.remotePath);
            row->appendRow({readOnly(itemText(entry), from.isEmpty() ? path : from + u'\n' + path),
                            readOnly(entry.quality), readOnly(entry.peer), where});
        }
        m_model->appendRow(sessionRow);
    }
    // The current session starts expanded: it's the one people look at.
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->item(row)->text().contains(tr("(this one)"))) {
            m_table->expand(m_model->index(row, 0));
        }
    }
    m_summary->setText(total == 0 ? tr("One session per launch. Downloads appear under the session they finished in, "
                                       "with where each file is now.")
                                  : QString());
    m_summary->setVisible(total == 0); // the counts are in the toolbar's subtitle
}

} // namespace selecta::app
