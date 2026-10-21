// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/AddTracksDialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

AddTracksDialog::AddTracksDialog(QWidget *parent)
    : QDialog(parent)
    , m_editor(new QPlainTextEdit(this))
{
    setObjectName(u"addTracks"_s);
    setWindowTitle(tr("Add Tracks"));
    setWindowModality(Qt::WindowModal); // a sheet on macOS
    resize(560, 420);

    auto *intro = new QLabel(tr("Paste the tracks you want, one per line, like <i>Artist - Title</i>. Typos are "
                                "fine. Spotify track, album and playlist links work too."),
                             this);
    intro->setWordWrap(true);

    m_editor->setObjectName(u"addTracks.text"_s);
    m_editor->setPlaceholderText(tr("Okuma Drift - Neon Rain\nHalden & Rue - Tidewater (Original Mix)\n"
                                    "https://open.spotify.com/playlist/…"));
    m_editor->setTabChangesFocus(true);
    m_editor->setAccessibleName(tr("Tracks"));
    QFont mono = m_editor->font();
    mono.setStyleHint(QFont::Monospace);
    m_editor->setFont(mono);

    auto *buttons = new QDialogButtonBox(this);
    auto *open = buttons->addButton(tr("Open Text File…"), QDialogButtonBox::ActionRole);
    open->setObjectName(u"addTracks.open"_s);
    buttons->addButton(QDialogButtonBox::Cancel);
    m_add = buttons->addButton(tr("Add Tracks"), QDialogButtonBox::AcceptRole);
    m_add->setObjectName(u"addTracks.add"_s);
    m_add->setDefault(true);
    connect(open, &QPushButton::clicked, this, &AddTracksDialog::openFile);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &AddTracksDialog::refreshButtons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_editor, 1);
    layout->addWidget(buttons);
    refreshButtons();
}

QString AddTracksDialog::text() const
{
    return m_editor->toPlainText();
}

void AddTracksDialog::setText(const QString &text)
{
    m_editor->setPlainText(text);
}

void AddTracksDialog::refreshButtons()
{
    m_add->setEnabled(!m_editor->toPlainText().trimmed().isEmpty());
}

void AddTracksDialog::openFile()
{
    const QString start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Open a Tracklist"), start, tr("Text files (*.txt *.m3u *.m3u8 *.csv)"));
    if (path.isEmpty()) {
        return;
    }
    // A file's path, shown as it is.
    const auto warn = [this](const QString &title, const QString &text) {
        QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
    };
    QFile file(path);
    if (QFileInfo(path).size() > kMaxFileBytes) {
        warn(tr("File Too Large"), tr("%1 is larger than a tracklist should be.").arg(path));
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        warn(tr("Can't Open the File"), file.errorString());
        return;
    }
    const QByteArray bytes = file.read(kMaxFileBytes);
    // UTF-8 when it decodes cleanly, otherwise Latin-1 (old Windows tracklists).
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(bytes);
    if (utf8.hasError()) {
        text = QString::fromLatin1(bytes);
    }
    QString current = m_editor->toPlainText();
    if (!current.isEmpty() && !current.endsWith(u'\n')) {
        current += u'\n';
    }
    m_editor->setPlainText(current + text);
}

} // namespace selecta::app
