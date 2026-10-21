// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/SharesView.h"

#include "app/Format.h"
#include "app/PeerMenu.h"
#include "core/EngineHost.h"

#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kColumnWidth = 720;

QLabel *text(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText); // folder names; the ratio's table is set rich
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString totals(core::TransferTotals totals)
{
    return totals.files == 1 ? SharesView::tr("1 file, %1").arg(format::size(totals.bytes))
                             : SharesView::tr("%1 files, %2").arg(totals.files).arg(format::size(totals.bytes));
}

} // namespace

SharesView::SharesView(core::EngineHost &engine, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_headline(text(u"shares.headline"_s, this))
    , m_folders(text(u"shares.folders"_s, this))
    , m_message(text(u"shares.message"_s, this))
    , m_activity(text(u"shares.activity"_s, this))
    , m_ratio(text(u"shares.ratio"_s, this))
    , m_guidance(text(u"shares.guidance"_s, this))
    , m_noUploads(text(u"shares.noUploads"_s, this))
    , m_uploads(new QTreeWidget(this))
{
    setObjectName(u"view.shares"_s);

    auto *column = new QWidget;
    column->setMaximumWidth(kColumnWidth);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(16);

    QFont headlineFont = m_headline->font();
    headlineFont.setPointSizeF(headlineFont.pointSizeF() + 3);
    headlineFont.setWeight(QFont::DemiBold);
    m_headline->setFont(headlineFont);
    layout->addWidget(m_headline);

    auto *what = new QGroupBox(tr("What you share"), column);
    auto *whatLayout = new QVBoxLayout(what);
    auto *explain = new QLabel(tr("Your collection, read-only: other Soulseek users can browse it, search it and "
                                  "download from it. Your Inbox and Selecta's own folders are never shared."),
                               what);
    explain->setWordWrap(true);
    explain->setForegroundRole(QPalette::PlaceholderText);
    whatLayout->addWidget(explain);
    whatLayout->addWidget(m_folders);
    m_message->setForegroundRole(QPalette::PlaceholderText);
    whatLayout->addWidget(m_message);
    auto *choose = new QPushButton(tr("Choose Folders…"), what);
    choose->setObjectName(u"shares.choose"_s);
    connect(choose, &QPushButton::clicked, this, &SharesView::settingsRequested);
    whatLayout->addWidget(choose, 0, Qt::AlignLeft);
    layout->addWidget(what);

    auto *ratio = new QGroupBox(tr("Give and take"), column);
    auto *ratioLayout = new QVBoxLayout(ratio);
    ratioLayout->addWidget(m_guidance);
    m_ratio->setTextFormat(Qt::RichText);
    ratioLayout->addWidget(m_ratio);
    layout->addWidget(ratio);

    auto *now = new QGroupBox(tr("Right now"), column);
    auto *nowLayout = new QVBoxLayout(now);
    nowLayout->addWidget(m_activity);
    m_noUploads->setText(tr("Nobody is downloading from you right now."));
    m_noUploads->setForegroundRole(QPalette::PlaceholderText);
    nowLayout->addWidget(m_noUploads);
    m_uploads->setObjectName(u"shares.uploads"_s);
    m_uploads->setHeaderLabels({tr("User"), tr("File"), tr("Progress")});
    m_uploads->setRootIsDecorated(false);
    m_uploads->setUniformRowHeights(true);
    m_uploads->setAlternatingRowColors(true);
    m_uploads->setFrameShape(QFrame::NoFrame);
    m_uploads->header()->setStretchLastSection(false);
    m_uploads->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_uploads->setMinimumHeight(120);
    // Who is downloading: browse them, write to them, or ban them (NET-8).
    m_uploads->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_uploads, &QTreeWidget::customContextMenuRequested, this, [this](QPoint position) {
        const QTreeWidgetItem *row = m_uploads->itemAt(position);
        if (!row) {
            return;
        }
        QMenu menu(this);
        PeerMenu::addActions(menu, row->text(0), m_engine,
                             {.browse = [this](const QString &peer) { Q_EMIT browseRequested(peer); },
                              .message = [this](const QString &peer) { Q_EMIT messageRequested(peer); }});
        menu.exec(m_uploads->viewport()->mapToGlobal(position));
    });
    nowLayout->addWidget(m_uploads);
    layout->addWidget(now);
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *centre = new QWidget;
    auto *centreLayout = new QHBoxLayout(centre);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    centreLayout->addWidget(column);
    centreLayout->addStretch(1);
    scroll->setWidget(centre);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(&m_engine, &core::EngineHost::shareStatusChanged, this, &SharesView::showStatus);
    connect(&m_engine, &core::EngineHost::uploadsChanged, this, &SharesView::showUploads);
    showStatus(m_engine.shareStatus());
    showUploads(m_engine.uploads());
}

QString SharesView::summary() const
{
    if (m_status.building) {
        return tr("Indexing what you share…");
    }
    if (m_status.files == 0) {
        return tr("Nothing shared");
    }
    return m_status.uploading > 0 ? tr("%1 files shared · %2 uploading").arg(m_status.files).arg(m_status.uploading)
                                  : tr("%1 files shared").arg(m_status.files);
}

void SharesView::showStatus(const core::ShareStatus &status)
{
    m_status = status;
    if (status.roots.isEmpty()) {
        m_headline->setText(tr("Nothing shared yet"));
    } else if (status.building) {
        m_headline->setText(tr("Indexing what you share…"));
    } else {
        m_headline->setText(status.files == 1
                                ? tr("Sharing 1 file")
                                : tr("Sharing %1 files in %2 folders").arg(status.files).arg(status.folders));
    }
    QStringList roots;
    for (const QString &root : status.roots) {
        roots.append(QDir::toNativeSeparators(root));
    }
    m_folders->setText(roots.isEmpty() ? tr("No collection folder yet.") : roots.join(u'\n'));
    m_message->setText(status.message);
    m_message->setVisible(!status.message.isEmpty());
    m_activity->setText(tr("%1 searches answered and %2 browses this session · %3 uploading, %4 waiting")
                            .arg(status.searchesAnswered)
                            .arg(status.browsed)
                            .arg(status.uploading)
                            .arg(status.queued));

    const core::RatioAssessment assessment = core::assessRatio(status);
    m_guidance->setText(assessment.guidance);
    const QString row = u"<tr><td style=\"padding-right:14px\">%1</td><td style=\"padding-right:14px\">%2</td>"
                        "<td>%3</td></tr>"_s;
    m_ratio->setText(u"<table>"_s + row.arg(QString(), tr("Uploaded"), tr("Downloaded")) +
                     row.arg(tr("This session"), totals(status.uploadedSession), totals(status.downloadedSession)) +
                     row.arg(tr("30 days"), totals(status.uploaded30Days), totals(status.downloaded30Days)) +
                     row.arg(tr("All time"), totals(status.uploadedAllTime), totals(status.downloadedAllTime)) +
                     u"</table>"_s);
    Q_EMIT summaryChanged();
}

void SharesView::showUploads(const QList<core::UploadEntry> &uploads)
{
    m_uploads->clear();
    for (const core::UploadEntry &upload : uploads) {
        const int percent = upload.size > 0 ? static_cast<int>(upload.sent * 100 / upload.size) : 0;
        auto *row = new QTreeWidgetItem(m_uploads);
        row->setText(0, upload.user);
        row->setText(1, upload.path.section(u'\\', -1));
        row->setToolTip(1, format::plainToolTip(upload.path)); // as the peer asked for it
        row->setText(2, u"%1 % of %2"_s.arg(percent).arg(format::size(static_cast<qint64>(upload.size))));
    }
    m_uploads->setVisible(!uploads.isEmpty());
    m_noUploads->setVisible(uploads.isEmpty());
}

} // namespace selecta::app
