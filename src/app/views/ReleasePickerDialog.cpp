// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/ReleasePickerDialog.h"

#include "app/Format.h"
#include "core/EngineHost.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

QString confidenceText(meta::Confidence confidence)
{
    switch (confidence) {
    case meta::Confidence::Exact:
        return ReleasePickerDialog::tr("exact");
    case meta::Confidence::Confident:
        return ReleasePickerDialog::tr("confident");
    case meta::Confidence::Likely:
        return ReleasePickerDialog::tr("likely");
    case meta::Confidence::Ambiguous:
        return ReleasePickerDialog::tr("uncertain");
    case meta::Confidence::None:
        break;
    }
    return ReleasePickerDialog::tr("unlikely");
}

} // namespace

ReleasePickerDialog::ReleasePickerDialog(core::EngineHost &engine, qint64 itemId, const QString &track, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_itemId(itemId)
    , m_status(new QLabel(tr("Looking it up…"), this))
    , m_list(new QListWidget(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Cancel, this))
{
    setObjectName(u"dialog.releasePicker"_s);
    setWindowTitle(tr("Choose a Release"));
    setWindowModality(Qt::WindowModal); // a sheet on macOS
    resize(560, 380);

    auto *heading = new QLabel(tr("Which release is “%1”?").arg(track), this);
    QFont font = heading->font();
    font.setWeight(QFont::DemiBold);
    heading->setFont(font);
    heading->setWordWrap(true);
    auto *note = new QLabel(tr("Its tags, label, genre and cover come from the release you choose."), this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    m_status->setObjectName(u"dialog.releasePicker.status"_s);
    m_status->setWordWrap(true);
    m_list->setObjectName(u"dialog.releasePicker.list"_s);
    m_list->setAlternatingRowColors(true);
    m_list->setAccessibleName(tr("Releases"));
    m_list->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_list->hide();

    QPushButton *apply = m_buttons->addButton(tr("Use This Release"), QDialogButtonBox::AcceptRole);
    apply->setObjectName(u"dialog.releasePicker.apply"_s);
    apply->setEnabled(false);
    connect(m_list, &QListWidget::currentRowChanged, this, [apply](int row) { apply->setEnabled(row >= 0); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::accepted, this, [this] {
        const int row = m_list->currentRow();
        if (row >= 0 && row < m_candidates.size()) {
            m_engine.applyInboxRelease(m_itemId, m_candidates.at(row).release);
        }
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(note);
    layout->addWidget(m_status);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_buttons);

    connect(&m_engine, &core::EngineHost::inboxReleasesFound, this, &ReleasePickerDialog::showCandidates);
    m_engine.findInboxReleases(itemId);
}

void ReleasePickerDialog::showCandidates(qint64 itemId, const QList<meta::Candidate> &candidates, const QString &error)
{
    if (itemId != m_itemId) {
        return;
    }
    m_candidates = candidates;
    m_list->clear();
    for (const meta::Candidate &candidate : candidates) {
        const meta::Release &release = candidate.release;
        QStringList facts{format::release(release)};
        if (!release.album.isEmpty() && release.album != release.title) {
            facts.append(release.album);
        }
        if (release.releaseDate.size() >= 4) {
            facts.append(release.releaseDate.left(4));
        }
        facts.append(u"%1 (%2)"_s.arg(meta::sourceName(release.source), confidenceText(candidate.confidence)));
        auto *row = new QListWidgetItem(facts.join(u" · "_s), m_list);
        row->setToolTip(candidate.reasons.join(u"\n"_s));
    }
    m_list->setVisible(!candidates.isEmpty());
    m_status->setVisible(candidates.isEmpty());
    m_status->setText(error.isEmpty() ? tr("No release matches it.") : error);
    if (!candidates.isEmpty()) {
        m_list->setCurrentRow(0);
        m_list->setFocus();
    }
}

} // namespace selecta::app
