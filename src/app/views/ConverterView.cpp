// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/ConverterView.h"

#include "app/Format.h"
#include "app/TickDelegate.h"
#include "app/models/ConverterModel.h"
#include "app/views/ConverterInspector.h"
#include "core/EngineHost.h"
#include "match/RemotePathParser.h"

#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using State = core::ConverterItem::State;

struct Target
{
    const char *id;
    const char *label;
};

// The formats CONV-1 offers, the DJ's usual choices first.
constexpr std::array kTargets{
    Target{"mp3-320", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "MP3 320 kbps CBR")},
    Target{"mp3-256", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "MP3 256 kbps CBR")},
    Target{"mp3-192", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "MP3 192 kbps CBR")},
    Target{"mp3-v0", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "MP3 V0 (VBR)")},
    Target{"mp3-v2", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "MP3 V2 (VBR)")},
    Target{"aiff", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "AIFF")},
    Target{"wav", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "WAV")},
    Target{"flac", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "FLAC")},
    Target{"alac", QT_TRANSLATE_NOOP("selecta::app::ConverterView", "ALAC (Apple Lossless)")},
};

constexpr std::array kArtwork{800, 1000, 600, 0};

// The formats that take a bit depth, and the rates of AppSettings::kConverterSampleRates (CONV-1); MP3 takes those
// of kConverterMp3SampleRates.
bool isLossless(convert::OutputFormat format)
{
    return format == convert::OutputFormat::Aiff || format == convert::OutputFormat::Wav ||
           format == convert::OutputFormat::Flac || format == convert::OutputFormat::Alac;
}

bool isAudio(const QString &path)
{
    return match::RemotePathParser::fileKind(QFileInfo(path).suffix().toLower()) == match::FileKind::Audio;
}

// Files as they are; folders give their audio files, one level of subfolders included (an album's CD1, CD2).
QStringList audioFilesIn(const QStringList &paths)
{
    QStringList files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isFile()) {
            files.append(info.absoluteFilePath());
            continue;
        }
        if (!info.isDir()) {
            continue;
        }
        QDirIterator walk(info.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
        QStringList found;
        while (walk.hasNext() && found.size() < 2000) {
            const QString file = walk.next();
            if (isAudio(file) && QDir(info.absoluteFilePath()).relativeFilePath(file).count(u'/') <= 1) {
                found.append(file);
            }
        }
        found.sort();
        files.append(found);
    }
    return files;
}

} // namespace

ConverterView::ConverterView(core::EngineHost &engine, AppSettings &settings, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_settings(settings)
    , m_model(new ConverterModel(this))
    , m_inspector(new ConverterInspector(this))
    , m_target(new QComboBox(this))
    , m_sampleRate(new QComboBox(this))
    , m_bitDepth(new QComboBox(this))
    , m_id3(new QComboBox(this))
    , m_artwork(new QComboBox(this))
    , m_destination(new QComboBox(this))
    , m_add(new QPushButton(tr("Add Files…"), this))
    , m_convert(new QPushButton(tr("Convert"), this))
    , m_stop(new QPushButton(tr("Stop"), this))
    , m_status(new QLabel(this))
    , m_pages(new QStackedWidget(this))
    , m_table(new QTreeView(this))
{
    setObjectName(u"view.converter"_s);
    setAcceptDrops(true);

    m_table->setObjectName(u"converter.items"_s);
    m_table->setAccessibleName(tr("Files to convert"));
    m_table->setModel(m_model);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setItemDelegateForColumn(ConverterModel::AnywayColumn, new TickDelegate(m_table));
    QHeaderView *header = m_table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(ConverterModel::ResultColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(ConverterModel::FileColumn, 260);
    m_table->setColumnWidth(ConverterModel::QualityColumn, 170);
    m_table->setColumnWidth(ConverterModel::AnywayColumn, 110);
    connect(m_table, &QTreeView::customContextMenuRequested, this, &ConverterView::contextMenu);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &ConverterView::showSelection);
    connect(m_model, &ConverterModel::confirmToggled, this,
            [this](qint64 id, bool yes) { m_engine.confirmConversion(id, yes); });

    auto *empty = new QLabel(tr("Drop audio files or folders here, or Add Files…, to make new files from them in "
                                "another format. Originals are never changed."),
                             this);
    empty->setObjectName(u"converter.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(empty);
    m_pages->addWidget(m_table);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(createBar());
    layout->addWidget(m_pages, 1);

    connect(&m_engine, &core::EngineHost::converterChanged, this, &ConverterView::refresh);
    connect(&m_engine, &core::EngineHost::conversionsFinished, this, [this](int done, int failed) {
        if (failed > 0) {
            m_status->setText(tr("%1 converted, %2 failed.").arg(done).arg(failed));
        } else {
            m_status->setText(done == 1 ? tr("1 file converted.") : tr("%1 files converted.").arg(done));
        }
    });
    m_engine.setConverterOptions(options());
    refresh();
}

QWidget *ConverterView::createBar()
{
    auto *bar = new QWidget(this);
    const AppSettings::ConverterState state = m_settings.converterState();

    m_target->setObjectName(u"converter.target"_s);
    for (const Target &target : kTargets) {
        m_target->addItem(tr(target.label), QString::fromLatin1(target.id));
    }
    m_target->setCurrentIndex(std::max(0, m_target->findData(state.target)));
    m_sampleRate->setObjectName(u"converter.sampleRate"_s);
    m_sampleRate->setAccessibleName(tr("Sample rate"));
    m_bitDepth->setObjectName(u"converter.bitDepth"_s);
    m_bitDepth->setAccessibleName(tr("Bit depth"));
    m_bitDepth->addItem(tr("Automatic bit depth"), 0);
    for (const int depth : AppSettings::kConverterBitDepths) {
        m_bitDepth->addItem(tr("%1-bit").arg(depth), depth);
    }
    m_losslessRate = state.sampleRate;
    m_losslessDepth = state.bitDepth;
    m_mp3Rate = state.mp3SampleRate;
    m_id3->setObjectName(u"converter.id3"_s);
    m_id3->setAccessibleName(tr("Tag format"));
    m_id3->addItem(tr("ID3v2.3 (best for DJ gear)"));
    m_id3->addItem(tr("ID3v2.4"));
    m_id3->setCurrentIndex(state.id3v24 ? 1 : 0);
    fitChoicesToTarget();
    m_artwork->setObjectName(u"converter.artwork"_s);
    m_artwork->setAccessibleName(tr("Cover size"));
    for (const int side : kArtwork) {
        m_artwork->addItem(side > 0 ? tr("Cover at most %1 px").arg(side) : tr("Cover as it is"), side);
    }
    m_artwork->setCurrentIndex(std::max(0, m_artwork->findData(state.artworkMax)));
    m_outputFolder = state.outputFolder;
    m_destination->setObjectName(u"converter.destination"_s);
    m_destination->addItem(tr("Next to the originals"));
    m_destination->addItem(m_outputFolder.isEmpty() ? tr("In a Folder…")
                                                    : tr("In %1").arg(QDir(m_outputFolder).dirName()));
    m_destination->setCurrentIndex(m_outputFolder.isEmpty() ? 0 : 1);
    m_destination->setToolTip(format::plainToolTip(m_outputFolder));

    m_add->setObjectName(u"converter.add"_s);
    m_convert->setObjectName(u"converter.convert"_s);
    m_convert->setDefault(true);
    m_stop->setObjectName(u"converter.stop"_s);
    m_status->setObjectName(u"converter.status"_s);
    m_status->setForegroundRole(QPalette::PlaceholderText);

    connect(m_target, &QComboBox::currentIndexChanged, this, [this] {
        fitChoicesToTarget();
        optionsEdited();
    });
    connect(m_sampleRate, &QComboBox::currentIndexChanged, this, [this] {
        const bool lossless = isLossless(targetFor(m_target->currentData().toString()).format);
        (lossless ? m_losslessRate : m_mp3Rate) = m_sampleRate->currentData().toInt();
        optionsEdited();
    });
    connect(m_bitDepth, &QComboBox::currentIndexChanged, this, [this] {
        m_losslessDepth = m_bitDepth->currentData().toInt();
        optionsEdited();
    });
    connect(m_id3, &QComboBox::currentIndexChanged, this, &ConverterView::optionsEdited);
    connect(m_artwork, &QComboBox::currentIndexChanged, this, &ConverterView::optionsEdited);
    connect(m_destination, &QComboBox::activated, this, [this](int index) {
        if (index == 1) {
            chooseFolder();
        } else {
            m_outputFolder.clear();
            optionsEdited();
        }
    });
    connect(m_add, &QPushButton::clicked, this, &ConverterView::addFiles);
    connect(m_convert, &QPushButton::clicked, this, [this] {
        m_status->clear();
        m_engine.startConversions(); // every file that can go; the menu converts the selection
    });
    connect(m_stop, &QPushButton::clicked, this, [this] { m_engine.cancelConversions(); });

    auto *first = new QHBoxLayout;
    first->setContentsMargins(0, 0, 0, 0);
    auto *convertTo = new QLabel(tr("Convert to"), bar);
    convertTo->setBuddy(m_target);
    first->addWidget(convertTo);
    first->addWidget(m_target);
    first->addWidget(m_sampleRate);
    first->addWidget(m_bitDepth);
    first->addWidget(m_id3);
    first->addWidget(m_artwork);
    first->addStretch(1);
    first->addWidget(m_add);
    first->addWidget(m_stop);
    first->addWidget(m_convert);
    auto *second = new QHBoxLayout;
    second->setContentsMargins(0, 0, 0, 0);
    auto *saveTo = new QLabel(tr("New files go"), bar);
    saveTo->setBuddy(m_destination);
    second->addWidget(saveTo);
    second->addWidget(m_destination);
    auto *note = new QLabel(tr("Originals are never changed, and no file is ever replaced."), bar);
    note->setForegroundRole(QPalette::PlaceholderText);
    second->addWidget(note);
    second->addStretch(1);
    second->addWidget(m_status);
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(16, 10, 16, 10);
    rows->setSpacing(8);
    rows->addLayout(first);
    rows->addLayout(second);
    return bar;
}

core::ConverterOptions ConverterView::options() const
{
    core::ConverterOptions options;
    options.target = targetFor(m_target->currentData().toString());
    if (isLossless(options.target.format)) {
        options.target.sampleRate = m_losslessRate;
        options.target.bitDepth = m_losslessDepth;
    } else if (options.target.format == convert::OutputFormat::Mp3) {
        options.target.sampleRate = m_mp3Rate;
    }
    options.id3 = m_id3->currentIndex() == 1 ? convert::Id3Version::V24 : convert::Id3Version::V23;
    options.artworkMax = m_artwork->currentData().toInt();
    options.outputFolder = m_outputFolder;
    return options;
}

convert::EncodeSettings ConverterView::targetFor(const QString &id)
{
    convert::EncodeSettings settings;
    if (id.startsWith(u"mp3-v"_s)) {
        settings.format = convert::OutputFormat::Mp3;
        settings.mp3.mode = convert::Mp3Mode::Vbr;
        settings.mp3.vbrQuality = id.mid(5).toInt();
    } else if (id.startsWith(u"mp3-"_s)) {
        settings.format = convert::OutputFormat::Mp3;
        settings.mp3.mode = convert::Mp3Mode::Cbr;
        settings.mp3.bitrateKbps = id.mid(4).toInt();
    } else if (id == u"aiff"_s) {
        settings.format = convert::OutputFormat::Aiff;
    } else if (id == u"wav"_s) {
        settings.format = convert::OutputFormat::Wav;
    } else if (id == u"flac"_s) {
        settings.format = convert::OutputFormat::Flac;
    } else if (id == u"alac"_s) {
        settings.format = convert::OutputFormat::Alac;
    }
    return settings;
}

void ConverterView::fitChoicesToTarget()
{
    const convert::OutputFormat format = targetFor(m_target->currentData().toString()).format;
    const bool lossless = isLossless(format);
    const QSignalBlocker blockRate(m_sampleRate);
    const QSignalBlocker blockDepth(m_bitDepth);
    // Each family has its own rates, and its own choice among them.
    m_sampleRate->clear();
    m_sampleRate->addItem(tr("Automatic rate"), 0);
    if (lossless) {
        for (const int rate : AppSettings::kConverterSampleRates) {
            m_sampleRate->addItem(convert::describeRate(rate), rate);
        }
    } else {
        for (const int rate : AppSettings::kConverterMp3SampleRates) {
            m_sampleRate->addItem(convert::describeRate(rate), rate);
        }
    }
    m_sampleRate->setCurrentIndex(std::max(0, m_sampleRate->findData(lossless ? m_losslessRate : m_mp3Rate)));
    m_sampleRate->setToolTip(
        lossless
            ? tr("Automatic keeps the source's rate up to 96 kHz, the most CDJ-3000 and CDJ-2000NXS2 players play; "
                 "a higher rate becomes 88.2 or 96 kHz.")
            : tr("Automatic keeps the source's rate when MP3 can store it (32, 44.1 or 48 kHz); any other becomes "
                 "44.1 kHz."));
    m_bitDepth->setEnabled(lossless);
    m_bitDepth->setCurrentIndex(lossless ? std::max(0, m_bitDepth->findData(m_losslessDepth)) : 0);
    m_bitDepth->setToolTip(lossless ? tr("Automatic keeps 16-bit sources at 16 bits and makes the others 24-bit. "
                                         "Reducing to 16 bits adds TPDF dither.")
                                    : tr("MP3 has no bit depth."));
    // ID3 doesn't apply to FLAC (Vorbis comments) or ALAC (MP4 atoms).
    m_id3->setEnabled(format != convert::OutputFormat::Flac && format != convert::OutputFormat::Alac);
    if (format == convert::OutputFormat::Flac) {
        m_id3->setToolTip(tr("FLAC keeps its tags as Vorbis comments."));
    } else if (format == convert::OutputFormat::Alac) {
        m_id3->setToolTip(tr("ALAC keeps its tags as MP4 atoms."));
    } else {
        m_id3->setToolTip({});
    }
}

void ConverterView::optionsEdited()
{
    AppSettings::ConverterState state;
    state.target = m_target->currentData().toString();
    state.sampleRate = m_losslessRate;
    state.bitDepth = m_losslessDepth;
    state.mp3SampleRate = m_mp3Rate;
    state.id3v24 = m_id3->currentIndex() == 1;
    state.artworkMax = m_artwork->currentData().toInt();
    state.outputFolder = m_outputFolder;
    m_settings.setConverterState(state);
    m_engine.setConverterOptions(options());
    showSelection();
    Q_EMIT summaryChanged();
}

void ConverterView::chooseFolder()
{
    auto *dialog = new QFileDialog(this, tr("Where New Files Go"), m_outputFolder);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly);
    dialog->setWindowModality(Qt::WindowModal);
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString &folder) {
        m_outputFolder = folder;
        m_destination->setItemText(1, tr("In %1").arg(QDir(folder).dirName()));
        m_destination->setToolTip(format::plainToolTip(folder));
        optionsEdited();
    });
    connect(dialog, &QFileDialog::rejected, this,
            [this] { m_destination->setCurrentIndex(m_outputFolder.isEmpty() ? 0 : 1); });
    dialog->open();
}

void ConverterView::addFiles()
{
    auto *dialog = new QFileDialog(this, tr("Add Files to Convert"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFiles);
    dialog->setNameFilter(tr("Audio files (*.flac *.mp3 *.wav *.aif *.aiff *.m4a *.ogg *.opus *.wv *.ape)"));
    dialog->setWindowModality(Qt::WindowModal);
    connect(dialog, &QFileDialog::filesSelected, this, [this](const QStringList &files) { addPaths(files); });
    dialog->open();
}

void ConverterView::addPaths(const QStringList &paths)
{
    const QStringList files = audioFilesIn(paths);
    if (!files.isEmpty()) {
        m_engine.addToConverter(files);
    }
}

void ConverterView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void ConverterView::dropEvent(QDropEvent *event)
{
    QStringList paths;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    addPaths(paths);
    event->acceptProposedAction();
}

void ConverterView::refresh()
{
    const qint64 current =
        m_table->currentIndex().isValid() ? m_table->currentIndex().data(ConverterModel::ItemIdRole).toLongLong() : 0;
    m_model->setItems(m_engine.converterItems());
    if (const int row = m_model->rowOf(current); row >= 0 && !m_table->currentIndex().isValid()) {
        m_table->setCurrentIndex(m_model->index(row, 0));
    }
    m_pages->setCurrentIndex(m_model->rowCount() == 0 ? 0 : 1);
    const QList<core::ConverterItem> &items = m_model->items();
    const bool running =
        std::ranges::any_of(items, [](const core::ConverterItem &item) { return item.state == State::Converting; });
    const auto convertible = static_cast<int>(
        std::ranges::count_if(items, [](const core::ConverterItem &item) { return item.isConvertible(); }));
    m_convert->setEnabled(convertible > 0);
    m_convert->setText(convertible > 1 ? tr("Convert %1").arg(convertible) : tr("Convert"));
    m_stop->setVisible(running);
    showSelection();
    Q_EMIT summaryChanged();
}

void ConverterView::showSelection()
{
    const QModelIndex current = m_table->currentIndex();
    m_inspector->showItem(current.isValid() ? m_model->itemAt(current.row()) : std::nullopt, options());
}

QList<qint64> ConverterView::selectedIds() const
{
    QList<qint64> ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        ids.append(row.data(ConverterModel::ItemIdRole).toLongLong());
    }
    return ids;
}

void ConverterView::contextMenu(QPoint position)
{
    const QList<qint64> ids = selectedIds();
    if (ids.isEmpty()) {
        return;
    }
    QMenu menu(this);
    menu.addAction(ids.size() == 1 ? tr("Convert") : tr("Convert %1 Files").arg(ids.size()), this,
                   [this, ids] { m_engine.startConversions(ids); });
    const std::optional<core::ConverterItem> first = m_model->itemAt(m_table->currentIndex().row());
    if (first && first->state == State::Done) {
        menu.addAction(tr("Show in Finder"), this,
                       [path = first->outputPath] { QProcess::startDetached(u"/usr/bin/open"_s, {u"-R"_s, path}); });
    }
    menu.addSeparator();
    menu.addAction(ids.size() == 1 ? tr("Remove from List") : tr("Remove %1 from List").arg(ids.size()), this,
                   [this, ids] { m_engine.removeFromConverter(ids); });
    menu.addAction(tr("Clear Finished"), this, [this] { m_engine.clearFinishedConversions(); });
    menu.exec(m_table->viewport()->mapToGlobal(position));
}

QString ConverterView::summary() const
{
    const QList<core::ConverterItem> &items = m_model->items();
    if (items.isEmpty()) {
        return tr("Drop files to convert");
    }
    // "AIFF, 24-bit, 96 kHz": the format with the choices made for it.
    const convert::EncodeSettings chosen = options().target;
    QString target = m_target->currentText();
    if (chosen.bitDepth > 0) {
        target += u", "_s + tr("%1-bit").arg(chosen.bitDepth);
    }
    if (chosen.sampleRate > 0) {
        target += u", "_s + convert::describeRate(chosen.sampleRate);
    }
    return items.size() == 1 ? tr("1 file → %1").arg(target) : tr("%1 files → %2").arg(items.size()).arg(target);
}

} // namespace selecta::app
