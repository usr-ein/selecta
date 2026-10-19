// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/ActivityView.h"

#include "app/models/ActivityModel.h"
#include "core/ActivityLog.h"
#include "core/EngineHost.h"
#include "governor/Limits.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

ActivityView::ActivityView(core::EngineHost &engine, storage::Database &db, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_db(db)
    , m_model(new ActivityModel(this))
    , m_pausedBanner(new QWidget(this))
    , m_budget(new QLabel(this))
    , m_table(new QTreeView(this))
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

    m_budget->setObjectName(u"activity.budget"_s);
    m_budget->setForegroundRole(QPalette::PlaceholderText);
    m_budget->setWordWrap(true);

    m_table->setObjectName(u"activity.log"_s);
    m_table->setModel(m_model);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->header()->setStretchLastSection(true);
    m_table->header()->setSectionResizeMode(ActivityModel::Time, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(ActivityModel::Operation, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(ActivityModel::Subject, QHeaderView::Interactive);
    m_table->setColumnWidth(ActivityModel::Subject, 280);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_pausedBanner);
    auto *budgetBox = new QVBoxLayout;
    budgetBox->setContentsMargins(16, 10, 16, 10);
    budgetBox->addWidget(m_budget);
    layout->addLayout(budgetBox);
    layout->addWidget(m_table, 1);

    // Follow the engine first, then load: a line logged in between arrives twice and the model drops the copy.
    connect(&m_engine, &core::EngineHost::activityRecorded, m_model, &ActivityModel::prepend);
    connect(&m_engine, &core::EngineHost::pausedChanged, this, &ActivityView::refreshState);
    connect(&m_engine, &core::EngineHost::searchBudgetChanged, this, &ActivityView::refreshState);
    reload();
    refreshState();
}

QString ActivityView::summary() const
{
    if (m_engine.isPaused()) {
        return tr("Network paused");
    }
    // No translator is loaded, so plurals are spelt out rather than left to %n.
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

} // namespace selecta::app
