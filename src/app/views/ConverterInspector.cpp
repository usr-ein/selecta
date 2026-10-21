// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/ConverterInspector.h"

#include "app/Inspector.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

namespace keys = convert::keys;

struct FieldName
{
    QStringView key;
    const char *label = nullptr;
    QStringView frame; // ID3v2.3
    QStringView atom;  // MP4, as TagLib maps it (the key: the atom rekordbox reads, conversion §3)
};

// The fields DJ software reads, in the order a tag editor shows them (CONV-4).
constexpr std::array kFields{
    FieldName{keys::Title, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Title"), u"TIT2", u"©nam"},
    FieldName{keys::Artist, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Artist"), u"TPE1", u"©ART"},
    FieldName{keys::Album, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Album"), u"TALB", u"©alb"},
    FieldName{keys::AlbumArtist, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Album artist"), u"TPE2",
              u"aART"},
    FieldName{keys::TrackNumber, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Track"), u"TRCK", u"trkn"},
    FieldName{keys::DiscNumber, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Disc"), u"TPOS", u"disk"},
    FieldName{keys::Date, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Year"), u"TYER", u"©day"},
    FieldName{keys::Genre, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Genre"), u"TCON", u"©gen"},
    FieldName{keys::Bpm, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "BPM"), u"TBPM", u"tmpo"},
    FieldName{keys::InitialKey, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Key"), u"TKEY",
              u"----:com.apple.iTunes:KEY"},
    FieldName{keys::Label, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Label"), u"TPUB",
              u"----:com.apple.iTunes:LABEL"},
    FieldName{keys::Isrc, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "ISRC"), u"TSRC",
              u"----:com.apple.iTunes:ISRC"},
    FieldName{keys::Composer, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Composer"), u"TCOM", u"©wrt"},
    FieldName{keys::Comment, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Comment"), u"COMM", u"©cmt"},
    FieldName{keys::CatalogNumber, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Catalogue number"),
              u"TXXX:CATALOGNUMBER", u"----:com.apple.iTunes:CATALOGNUMBER"},
    FieldName{keys::EncodedBy, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Encoded by"), u"TENC", u"©enc"},
    FieldName{keys::Encoding, QT_TRANSLATE_NOOP("selecta::app::ConverterInspector", "Encoder settings"), u"TSSE",
              u"©too"},
};

QLabel *body(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

ConverterInspector::ConverterInspector(QWidget *parent)
    : QWidget(parent)
    , m_pages(new QStackedWidget(this))
    , m_title(body(u"inspector.converter.title"_s, this))
    , m_subtitle(body(u"inspector.converter.subtitle"_s, this))
    , m_cover(body(u"inspector.converter.cover"_s, this))
    , m_notices(body(u"inspector.converter.notices"_s, this))
    , m_tags(body(u"inspector.converter.tags"_s, this))
    , m_explain(body(u"inspector.converter.explain"_s, this))
    , m_reveal(new QPushButton(tr("Show in Finder"), this))
{
    setObjectName(u"inspector.converter"_s);
    auto *empty = new QLabel(tr("Select a file to see how its tags and cover are written in the new format."), this);
    empty->setObjectName(u"inspector.converter.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);

    auto *details = new QWidget;
    auto *layout = new QVBoxLayout(details);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
    m_title->setFont(titleFont);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    m_cover->setForegroundRole(QPalette::PlaceholderText);
    m_explain->setForegroundRole(QPalette::PlaceholderText);
    m_reveal->setObjectName(u"inspector.converter.reveal"_s);
    connect(m_reveal, &QPushButton::clicked, this, [this] {
        if (!m_output.isEmpty()) {
            QProcess::startDetached(u"/usr/bin/open"_s, {u"-R"_s, m_output});
        }
    });
    auto *revealRow = new QHBoxLayout;
    revealRow->setContentsMargins(0, 0, 0, 0);
    revealRow->addWidget(m_reveal);
    revealRow->addStretch(1);

    m_tags->setTextFormat(Qt::RichText); // each field: its name and tag in grey, the value below

    layout->addWidget(m_title);
    layout->addWidget(m_subtitle);
    layout->addLayout(revealRow);
    layout->addWidget(m_notices);
    layout->addWidget(m_cover);
    layout->addWidget(m_tags);
    layout->addWidget(m_explain);
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(details);
    Inspector::fitToWidth(scroll);
    m_pages->addWidget(scroll);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);
}

QString ConverterInspector::writtenAs(const QString &key, const core::ConverterOptions &options)
{
    if (options.target.format == convert::OutputFormat::Flac) {
        return key; // a Vorbis comment of the same name
    }
    const bool mp4 = options.target.format == convert::OutputFormat::Alac;
    const bool v24 = options.id3 == convert::Id3Version::V24;
    for (const FieldName &field : kFields) {
        if (field.key == key) {
            if (mp4) {
                return field.atom.toString();
            }
            return field.key == keys::Date && v24 ? u"TDRC"_s : field.frame.toString();
        }
    }
    return (mp4 ? u"----:com.apple.iTunes:"_s : u"TXXX:"_s) + key; // a free-form atom, a TXXX frame
}

void ConverterInspector::showItem(const std::optional<core::ConverterItem> &item, const core::ConverterOptions &options)
{
    if (!item) {
        m_pages->setCurrentIndex(0);
        return;
    }
    m_pages->setCurrentIndex(1);
    m_title->setText(QFileInfo(item->path).fileName());
    const QString format = convert::describeSettings(options.target);
    QString tagFormat = options.id3 == convert::Id3Version::V24 ? tr("ID3v2.4") : tr("ID3v2.3");
    if (options.target.format == convert::OutputFormat::Flac) {
        tagFormat = tr("Vorbis comments");
    } else if (options.target.format == convert::OutputFormat::Alac) {
        tagFormat = tr("MP4 atoms");
    }
    m_subtitle->setText(item->quality.isEmpty() ? tr("→ %1 · %2").arg(format, tagFormat)
                                                : tr("%1 → %2 · %3").arg(item->quality, format, tagFormat));
    m_output = item->state == core::ConverterItem::State::Done ? item->outputPath : QString();
    m_reveal->setVisible(!m_output.isEmpty());
    m_notices->setText(item->state == core::ConverterItem::State::Failed ? item->message : item->notices.join(u'\n'));
    m_notices->setVisible(!m_notices->text().isEmpty());
    m_cover->setText(item->cover.isEmpty()
                         ? tr("No cover in the file or its folder.")
                         : tr("Cover: %1, embedded as the front cover%2.")
                               .arg(item->cover, options.artworkMax > 0 ? tr(", at most %1 px").arg(options.artworkMax)
                                                                        : QString()));

    QStringList rows;
    QStringList shown;
    const QString grey = palette().color(QPalette::PlaceholderText).name();
    const auto addRow = [&](const QString &key, const QString &label) {
        const QString value = item->tags.values(key).join(u", "_s);
        if (value.isEmpty()) {
            return;
        }
        const QString frame = writtenAs(key, options);
        QString caption = frame == label ? label.toHtmlEscaped() : tr("%1 · %2").arg(label, frame).toHtmlEscaped();
        if (item->added.contains(key)) {
            caption += u" · "_s + tr("added from the names").toHtmlEscaped();
        }
        rows.append(u"<span style=\"color:%1\">%2</span><br>%3"_s.arg(grey, caption, value.toHtmlEscaped()));
    };
    for (const FieldName &field : kFields) {
        addRow(field.key.toString(), tr(field.label));
        shown.append(field.key.toString());
    }
    const QStringList others = item->tags.keys();
    for (const QString &key : others) {
        if (!shown.contains(key)) {
            addRow(key, key);
        }
    }
    m_tags->setText(rows.isEmpty() ? tr("No tags.") : rows.join(u"<br><br>"_s));
    if (options.target.format == convert::OutputFormat::Flac) {
        m_explain->setText(tr("FLAC keeps the tags as Vorbis comments, under the same names."));
    } else if (options.target.format == convert::OutputFormat::Alac) {
        m_explain->setText(tr("ALAC files (.m4a) keep their tags as MP4 atoms, as iTunes writes them, and the cover "
                              "as a covr atom. The key goes to ----:com.apple.iTunes:KEY, where rekordbox reads it. "
                              "Track and disc numbers must be numbers: a vinyl side like A1 is left out."));
    } else if (options.id3 == convert::Id3Version::V24) {
        m_explain->setText(
            tr("ID3v2.4 writes the year as TDRC and allows UTF-8; some older DJ players ignore v2.4 tags."));
    } else {
        m_explain->setText(tr("ID3v2.3 writes the year as TYER and uses UTF-16 text, which older CDJs and rekordbox "
                              "read reliably."));
    }
}

} // namespace selecta::app
