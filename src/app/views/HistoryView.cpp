// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/HistoryView.h"

#include "storage/SessionLog.h"

#include <QDateTime>
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

QString formatTime(qint64 msSinceEpoch)
{
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(msSinceEpoch), QLocale::ShortFormat);
}

QString formatDuration(qint64 ms)
{
    const qint64 minutes = std::max<qint64>(0, ms) / 60'000;
    if (minutes < 60) {
        return HistoryView::tr("%n min", nullptr, static_cast<int>(minutes));
    }
    return HistoryView::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

} // namespace

HistoryView::HistoryView(storage::SessionLog &sessions, int currentSession, QWidget *parent)
    : QWidget(parent)
    , m_sessions(sessions)
    , m_currentSession(currentSession)
    , m_model(new QStandardItemModel(this))
    , m_table(new QTreeView(this))
    , m_summary(new QLabel(this))
{
    setObjectName(u"view.history"_s);
    m_summary->setObjectName(u"history.summary"_s);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    m_summary->setWordWrap(true);

    m_model->setHorizontalHeaderLabels({tr("Session"), tr("Started"), tr("Ended"), tr("Length")});
    m_table->setObjectName(u"history.sessions"_s);
    m_table->setModel(m_model);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->header()->setStretchLastSection(true);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *summaryBox = new QVBoxLayout;
    summaryBox->setContentsMargins(16, 10, 16, 10);
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
    m_model->removeRows(0, m_model->rowCount());
    const QList<storage::SessionRecord> sessions = m_sessions.recent(kSessionsShown);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const storage::SessionRecord &session : sessions) {
        const bool isCurrent = session.number == m_currentSession;
        QString ended;
        QString length;
        if (session.endedAtMs) {
            ended = formatTime(*session.endedAtMs);
            length = formatDuration(*session.endedAtMs - session.startedAtMs);
        } else if (isCurrent) {
            ended = tr("Running");
            length = formatDuration(now - session.startedAtMs);
        } else {
            ended = tr("Didn't quit cleanly");
        }
        const QList<QStandardItem *> row{
            new QStandardItem(isCurrent ? tr("%1 (this one)").arg(session.number) : QString::number(session.number)),
            new QStandardItem(formatTime(session.startedAtMs)), new QStandardItem(ended), new QStandardItem(length)};
        m_model->appendRow(row);
    }
    for (int column = 0; column < m_model->columnCount() - 1; ++column) {
        m_table->resizeColumnToContents(column);
    }
    m_summary->setText(tr("Downloads will be listed here by session once Selecta can download (milestone M4). "
                          "Sessions are already recorded: one per launch."));
}

} // namespace selecta::app
