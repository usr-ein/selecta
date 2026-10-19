// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Sidebar.h"

#include "app/NavDelegate.h"
#include "app/NavModel.h"
#include "app/SourceListView.h"

#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kDotSize = 8;
const QColor kOnlineGreen(0x34, 0xC7, 0x59);

} // namespace

StatusDot::StatusDot(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kDotSize, kDotSize);
    setAccessibleName(tr("Offline"));
}

void StatusDot::setOnline(bool online)
{
    if (m_online == online) {
        return;
    }
    m_online = online;
    setAccessibleName(online ? tr("Online") : tr("Offline"));
    update();
}

void StatusDot::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_online ? kOnlineGreen : palette().color(QPalette::PlaceholderText));
    painter.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}

Sidebar::Sidebar(NavModel *model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_list(new SourceListView(this))
    , m_statusDot(new StatusDot(this))
    , m_statusText(new QLabel(this))
{
    setObjectName(u"sidebar"_s);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setMinimumWidth(180);

    m_list->setObjectName(u"sidebar.list"_s);
    m_list->setModel(m_model);
    m_list->setItemDelegate(new NavDelegate(m_list));
    m_list->setAccessibleName(tr("Sidebar"));

    m_statusDot->setObjectName(u"sidebar.statusDot"_s);
    m_statusText->setObjectName(u"sidebar.status"_s);
    m_statusText->setForegroundRole(QPalette::PlaceholderText);

    auto *status = new QHBoxLayout;
    status->setContentsMargins(16, 8, 16, 10);
    status->setSpacing(6);
    status->addWidget(m_statusDot);
    status->addWidget(m_statusText, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_list, 1);
    layout->addLayout(status);

    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
        if (m_selecting || !current.isValid()) {
            return;
        }
        const QString id = m_model->idAt(current.row());
        if (!id.isEmpty()) {
            Q_EMIT navigationRequested(id);
        }
    });

    setConnectionStatus(tr("Not connected"), false);
}

void Sidebar::select(const QString &id)
{
    const int row = m_model->rowOf(id);
    m_selecting = true;
    if (row < 0) {
        m_list->selectionModel()->clear();
    } else {
        m_list->selectionModel()->setCurrentIndex(m_model->index(row), QItemSelectionModel::ClearAndSelect);
    }
    m_selecting = false;
}

QString Sidebar::currentId() const
{
    const QModelIndex current = m_list->currentIndex();
    return current.isValid() ? m_model->idAt(current.row()) : QString();
}

void Sidebar::setConnectionStatus(const QString &text, bool online)
{
    m_statusText->setText(text);
    m_statusDot->setOnline(online);
}

} // namespace selecta::app
