// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/EditTagsDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

namespace keys = convert::keys;

} // namespace

EditTagsDialog::EditTagsDialog(const QString &fileName, const convert::MetadataModel &tags, QWidget *parent)
    : QDialog(parent)
    , m_original(tags)
{
    setObjectName(u"dialog.editTags"_s);
    setWindowTitle(tr("Edit Tags"));
    setWindowModality(Qt::WindowModal); // a sheet on macOS

    auto *heading = new QLabel(tr("Tags of “%1”").arg(fileName), this);
    QFont font = heading->font();
    font.setWeight(QFont::DemiBold);
    heading->setFont(font);
    heading->setWordWrap(true);
    auto *note = new QLabel(tr("Saved into the file in your Inbox, which is renamed to match. Several artists: "
                               "separate them with commas."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const std::array fields{
        std::pair{keys::Title, tr("Title")},       std::pair{keys::Artist, tr("Artists")},
        std::pair{keys::Album, tr("Album")},       std::pair{keys::AlbumArtist, tr("Album artist")},
        std::pair{keys::Date, tr("Release date")}, std::pair{keys::TrackNumber, tr("Track")},
        std::pair{keys::Genre, tr("Genre")},
    };
    for (const auto &[key, label] : fields) {
        auto *edit = new QLineEdit(tags.values(key).join(u", "_s), this);
        edit->setObjectName(u"dialog.editTags."_s + key.toString().toLower());
        edit->setClearButtonEnabled(true);
        edit->setMinimumWidth(320);
        form->addRow(label, edit);
        m_fields.insert(key.toString(), edit);
    }
    m_fields.value(keys::Date.toString())->setPlaceholderText(tr("2024 or 2024-06-09"));
    m_fields.value(keys::TrackNumber.toString())->setPlaceholderText(tr("3 or 3/12"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Without a title the file would have no name.
    const auto validate = [this, buttons] {
        buttons->button(QDialogButtonBox::Save)
            ->setEnabled(!m_fields.value(keys::Title.toString())->text().trimmed().isEmpty());
    };
    connect(m_fields.value(keys::Title.toString()), &QLineEdit::textChanged, this, validate);
    validate();

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(note);
    layout->addSpacing(6);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

convert::MetadataModel EditTagsDialog::changes() const
{
    convert::MetadataModel changes;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const QString text = it.value()->text().trimmed();
        if (text.isEmpty()) {
            continue;
        }
        // Artists are written ", "-joined (ID3v2.3, D30): one value with the commas kept, as Spotify's are.
        if (text != m_original.values(it.key()).join(u", "_s)) {
            changes.set(it.key(), text, convert::Provenance::User);
        }
    }
    return changes;
}

QStringList EditTagsDialog::cleared() const
{
    QStringList cleared;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        if (it.value()->text().trimmed().isEmpty() && m_original.contains(it.key())) {
            cleared.append(it.key());
        }
    }
    return cleared;
}

} // namespace selecta::app
