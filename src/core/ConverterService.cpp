// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ConverterService.h"

#include "convert/ArtworkFinder.h"
#include "convert/ArtworkProcessor.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagPolicy.h"
#include "convert/TagReader.h"
#include "convert/Transcoder.h"
#include "library/FileOps.h"
#include "library/Quarantine.h"
#include "match/RemotePathParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QThread>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::core {

namespace {

Q_LOGGING_CATEGORY(lcConverter, "selecta.core.converter")

namespace keys = convert::keys;
using convert::Provenance;
using State = ConverterItem::State;

constexpr qint64 kMaxCoverBytes = qint64{20} * 1024 * 1024;
constexpr qint64 kProgressPaceMs = 150;

bool isImage(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == u"jpg"_s || suffix == u"jpeg"_s || suffix == u"png"_s;
}

QByteArray readSmallFile(const QString &path)
{
    QFile file(path);
    if (file.size() > kMaxCoverBytes || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.read(kMaxCoverBytes);
}

} // namespace

// What reading a file found.
struct ConverterService::Source
{
    convert::SourceQuality quality;
    convert::MetadataModel tags; // its own, then what its folder and name add
    QStringList added;
    QString cover;          // where the cover comes from
    QByteArray folderCover; // the folder's image, when the file has none
    QString error;          // it can't be read
};

namespace {

// CONV-5: the fields the file lacks, from its folder's and its own name.
void supplementFromNames(const QString &path, convert::MetadataModel &tags, QStringList &added)
{
    const QFileInfo file(path);
    const QString folder = file.absoluteDir().dirName();
    const QString parent = QFileInfo(file.absolutePath()).absoluteDir().dirName();
    // The parser reads Soulseek paths: the last folders and the file, joined with backslashes.
    const QString virtualPath = parent + u'\\' + folder + u'\\' + file.fileName();
    const match::ParsedRemote parsed = match::RemotePathParser::parse(virtualPath);
    if (parsed.unparseable) {
        return;
    }
    const auto fill = [&tags, &added](QStringView key, const QString &value) {
        if (!value.isEmpty() && !tags.contains(key)) {
            tags.set(key, value, Provenance::FileName);
            added.append(key.toString());
        }
    };
    fill(keys::Title, parsed.title.value);
    fill(keys::Artist, parsed.artist.isSet() ? parsed.artist.value : parsed.albumArtist.value);
    fill(keys::Album, parsed.album.value);
    fill(keys::AlbumArtist, parsed.albumArtist.value);
    fill(keys::TrackNumber, parsed.track.isSet() ? QString::number(parsed.track.value) : QString());
    fill(keys::DiscNumber, parsed.disc.isSet() ? QString::number(parsed.disc.value) : QString());
    fill(keys::Date, parsed.year.isSet() ? QString::number(parsed.year.value) : QString());
}

// CONV-5: the folder's cover, for a file without one.
QByteArray folderCover(const QString &path, QString *name)
{
    const QDir folder = QFileInfo(path).absoluteDir();
    QList<convert::CoverCandidate> candidates;
    QStringList paths;
    QDirIterator images(folder.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    while (images.hasNext() && candidates.size() < 50) {
        const QString image = images.next();
        const QString relative = folder.relativeFilePath(image);
        if (!isImage(image) || relative.count(u'/') > 1) {
            continue;
        }
        const QByteArray bytes = readSmallFile(image);
        const std::optional<convert::ImageFacts> facts = bytes.isEmpty() ? std::nullopt : convert::inspectImage(bytes);
        candidates.append({.relativePath = relative, .size = facts ? facts->size : QSize()});
        paths.append(image);
    }
    const std::optional<convert::CoverChoice> choice = convert::chooseFolderCover(candidates);
    if (!choice || choice->index < 0 || choice->index >= paths.size()) {
        return {};
    }
    *name = candidates.at(choice->index).relativePath;
    return readSmallFile(paths.at(choice->index));
}

std::shared_ptr<const ConverterService::Source> readSource(const QString &path, const audio::CancellationFlag *cancel)
{
    auto source = std::make_shared<ConverterService::Source>();
    convert::Result<convert::SourceQuality> quality = convert::probeSourceQuality(path, true, cancel);
    if (!quality) {
        // The checks need the whole file; without them the stream facts still say what it is.
        quality = convert::probeSourceQuality(path, false, cancel);
    }
    if (!quality) {
        source->error = quality.error().message;
        return source;
    }
    source->quality = quality.takeValue();
    if (convert::Result<convert::MetadataModel> tags = convert::readTags(path); tags) {
        source->tags = tags.takeValue();
    }
    supplementFromNames(path, source->tags, source->added);
    if (source->tags.frontCover()) {
        source->cover = QCoreApplication::translate("selecta::core::ConverterService", "Embedded picture");
    } else {
        QString name;
        source->folderCover = folderCover(path, &name);
        if (!source->folderCover.isEmpty()) {
            source->cover =
                QCoreApplication::translate("selecta::core::ConverterService", "%1 from the folder").arg(name);
        }
    }
    return source;
}

QString sizeWords(double factor)
{
    if (factor <= 0.0) {
        return {};
    }
    if (factor >= 1.15) {
        return QCoreApplication::translate("selecta::core::ConverterService", "about %1× larger")
            .arg(factor, 0, 'f', factor < 10.0 ? 1 : 0);
    }
    if (factor <= 0.87) {
        const double smaller = 1.0 / factor;
        return QCoreApplication::translate("selecta::core::ConverterService", "about %1× smaller")
            .arg(smaller, 0, 'f', smaller < 10.0 ? 1 : 0);
    }
    return QCoreApplication::translate("selecta::core::ConverterService", "about the same size");
}

// What a conversion needs, copied from the converter's entry (which may go while it runs).
struct ConversionJob
{
    QString path;
    std::shared_ptr<const ConverterService::Source> source;
    ConverterOptions settings;
    QString stagingDir;
    QString encodedBy;
};

struct ConversionResult
{
    bool ok = false;
    QString outputPath;
    QString error;
};

// The file's tags with its cover: its own, else its folder's, sized for DJ players.
convert::MetadataModel tagsWithCover(const ConverterService::Source &source, int artworkMax)
{
    convert::MetadataModel tags = source.tags;
    const std::optional<convert::Picture> own = tags.frontCover();
    const QByteArray image = own ? own->data : source.folderCover;
    if (image.isEmpty()) {
        return tags;
    }
    convert::ArtworkOptions artwork;
    artwork.maxDimension = artworkMax > 0 ? artworkMax : 10000;
    if (const convert::Result<convert::ProcessedArtwork> processed = convert::processArtwork(image, artwork);
        processed) {
        convert::Picture picture;
        picture.data = processed.value().data;
        picture.mimeType = processed.value().mimeType;
        picture.provenance = own ? own->provenance : Provenance::Folder;
        tags.setFrontCover(picture);
    }
    return tags;
}

// One conversion: made and verified in staging (CONV-7), then placed next to the original or in the chosen folder as
// a new file, never in place of another (CONV-3). Blocking: a worker's.
ConversionResult convertFile(const ConversionJob &job, const audio::CancellationFlag *cancel,
                             const convert::TranscodeProgress &progress)
{
    const convert::OutputFormat format = job.settings.target.format;
    const bool keep = format == convert::OutputFormat::KeepAudio;
    const QFileInfo original(job.path);
    const QString name =
        original.completeBaseName() + u'.' + (keep ? original.suffix() : convert::fileExtension(format));
    QDir(job.stagingDir).removeRecursively();
    if (!QDir().mkpath(job.stagingDir)) {
        return {.ok = false,
                .outputPath = {},
                .error = QCoreApplication::translate("selecta::core::ConverterService", "Selecta can't write to %1.")
                             .arg(QDir::toNativeSeparators(job.stagingDir))};
    }
    convert::TranscodeRequest request;
    request.sourcePath = job.path;
    request.outputPath = QDir(job.stagingDir).filePath(name);
    request.settings = job.settings.target;
    request.tags = tagsWithCover(*job.source, job.settings.artworkMax);
    request.policy.transcoding = !keep;
    request.policy.encodedBy = job.encodedBy;
    if (format == convert::OutputFormat::Mp3) {
        request.policy.encoderSettings = convert::lameSettingsString(job.settings.target.mp3);
    }
    request.tagOptions.id3Version = job.settings.id3;
    const convert::Result<convert::TranscodeReport> report = convert::transcode(request, cancel, progress);
    if (!report) {
        QDir(job.stagingDir).removeRecursively();
        return {.ok = false, .outputPath = {}, .error = report.error().message};
    }
    // A file made from a download is still one: it keeps the original's quarantine attribute (NFR-4).
    if (QString why; !library::quarantine::copy(job.path, report.value().outputPath, &why)) {
        qCWarning(lcConverter).noquote() << why;
    }
    const QString folder = job.settings.outputFolder.isEmpty() ? original.absolutePath() : job.settings.outputFolder;
    const library::FileOps fileOps({}, {});
    const library::FileResult placed = fileOps.placeNewFile(report.value().outputPath, QDir(folder).filePath(name));
    QDir(job.stagingDir).removeRecursively();
    return {.ok = placed.ok, .outputPath = placed.path, .error = placed.error};
}

} // namespace

ConverterService::ConverterService(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    qRegisterMetaType<ConverterItem>();
    qRegisterMetaType<QList<ConverterItem>>();
    qRegisterMetaType<ConverterOptions>();
    const int workers =
        m_options.maxConcurrent > 0 ? m_options.maxConcurrent : std::clamp(QThread::idealThreadCount() / 2, 1, 4);
    m_pool.setMaxThreadCount(workers);
    m_pool.setObjectName(u"selecta-converter"_s);
}

ConverterService::~ConverterService()
{
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.cancel) {
            entry.cancel->cancel();
        }
    }
    m_pool.waitForDone(); // workers delete their staged files when cancelled
}

void ConverterService::add(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        const bool listed = std::ranges::any_of(m_entries, [&absolute](const Entry &entry) {
            return entry.item.path == absolute && !entry.item.isFinished();
        });
        if (listed || !QFileInfo(absolute).isFile()) {
            continue;
        }
        Entry entry;
        entry.item.id = m_nextId++;
        entry.item.path = absolute;
        entry.item.state = State::Reading;
        entry.cancel = std::make_shared<audio::CancellationFlag>();
        const qint64 id = entry.item.id;
        m_pool.start([this, id, absolute, cancel = entry.cancel] {
            post(Message{.kind = Message::Kind::Read,
                         .id = id,
                         .source = readSource(absolute, cancel.get()),
                         .progress = 0.0,
                         .ok = false,
                         .outputPath = {},
                         .error = {}});
        });
        m_entries.insert(id, std::move(entry));
        m_order.append(id);
    }
    publish();
}

void ConverterService::remove(const QList<qint64> &ids)
{
    for (const qint64 id : ids) {
        const auto found = m_entries.find(id);
        if (found == m_entries.end()) {
            continue;
        }
        if (found->cancel) {
            found->cancel->cancel(); // a running conversion stops and cleans up; its result is ignored
        }
        m_entries.erase(found);
        m_order.removeAll(id);
    }
    publish();
}

void ConverterService::clearFinished()
{
    QList<qint64> finished;
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.item.isFinished()) {
            finished.append(entry.item.id);
        }
    }
    remove(finished);
}

void ConverterService::setOptions(const ConverterOptions &options)
{
    if (options == m_settings) {
        return;
    }
    m_settings = options;
    for (Entry &entry : m_entries) {
        if (entry.item.state == State::Ready) {
            entry.item.confirmed = false; // a new target is a new question
            advise(entry);
        }
    }
    publish();
}

void ConverterService::confirm(qint64 id, bool yes)
{
    const auto found = m_entries.find(id);
    if (found == m_entries.end() || !found->item.askFirst) {
        return;
    }
    found->item.confirmed = yes;
    publish();
}

void ConverterService::advise(Entry &entry) const
{
    ConverterItem &item = entry.item;
    item.notices.clear();
    item.askFirst = false;
    item.refused = false;
    if (!entry.source) {
        return;
    }
    const convert::SourceQuality &quality = entry.source->quality;
    // The tags exactly as they'll be written: encoder fields and the like go when the audio is re-encoded (D-7).
    convert::TagPolicyOptions policy;
    policy.transcoding = m_settings.target.format != convert::OutputFormat::KeepAudio;
    policy.encodedBy = m_options.encodedBy;
    if (m_settings.target.format == convert::OutputFormat::Mp3) {
        policy.encoderSettings = convert::lameSettingsString(m_settings.target.mp3);
    }
    item.tags = convert::applyTagPolicy(entry.source->tags, policy);
    const convert::Advice advice = convert::advise(quality, m_settings.target, convert::AdviceContext::Manual);
    for (const convert::Notice notice : advice.notices) {
        const convert::NoticeText text = convert::describeNotice(notice, quality, m_settings.target, advice);
        item.notices.append(text.detail.isEmpty() ? text.shortText : text.shortText + u": "_s + text.detail);
    }
    item.askFirst = advice.action == convert::Action::Ask;
    item.refused = advice.action == convert::Action::Refuse;
    if (item.refused || item.askFirst) {
        item.verdict =
            item.notices.isEmpty()
                ? QString()
                : convert::describeNotice(advice.notices.constFirst(), quality, m_settings.target, advice).shortText;
        return;
    }
    QStringList parts{convert::describeSettings(m_settings.target)};
    if (const QString size = sizeWords(advice.sizeFactor); !size.isEmpty()) {
        parts.append(size);
    }
    item.verdict = parts.join(u" · "_s);
}

void ConverterService::start(const QList<qint64> &ids)
{
    QList<qint64> chosen = ids.isEmpty() ? m_order : ids;
    bool any = false;
    for (const qint64 id : std::as_const(chosen)) {
        const auto found = m_entries.find(id);
        if (found == m_entries.end() || !found->item.isConvertible() || !found->source) {
            continue;
        }
        Entry &entry = *found;
        if (!any &&
            std::ranges::none_of(m_entries, [](const Entry &other) { return other.item.state == State::Converting; })) {
            m_runDone = 0; // a new run
            m_runFailed = 0;
        }
        any = true;
        entry.item.state = State::Converting;
        entry.item.progress = 0.0;
        entry.item.message.clear();
        entry.cancel = std::make_shared<audio::CancellationFlag>();

        // Everything the worker needs, copied: the entry may go while it runs.
        ConversionJob job{.path = entry.item.path,
                          .source = entry.source,
                          .settings = m_settings,
                          .stagingDir = QDir(m_options.stagingDir).filePath(u"converter/%1"_s.arg(id)),
                          .encodedBy = m_options.encodedBy};
        m_pool.start([this, id, job = std::move(job), cancel = entry.cancel] {
            QElapsedTimer pace;
            pace.start();
            const ConversionResult result = convertFile(job, cancel.get(), [this, id, &pace](double fraction) {
                if (pace.elapsed() >= kProgressPaceMs) {
                    pace.restart();
                    post(Message{.kind = Message::Kind::Progress,
                                 .id = id,
                                 .source = {},
                                 .progress = fraction,
                                 .ok = false,
                                 .outputPath = {},
                                 .error = {}});
                }
            });
            post(Message{.kind = Message::Kind::Converted,
                         .id = id,
                         .source = {},
                         .progress = 1.0,
                         .ok = result.ok,
                         .outputPath = result.outputPath,
                         .error = result.error});
        });
    }
    if (any) {
        publish();
    }
}

void ConverterService::cancel()
{
    for (Entry &entry : m_entries) {
        if (entry.item.state == State::Converting) {
            if (entry.cancel) {
                entry.cancel->cancel();
            }
            entry.item.state = State::Cancelled;
            entry.item.message = tr("Cancelled");
        }
    }
    publish();
}

void ConverterService::post(Message message)
{
    bool schedule = false;
    {
        const std::scoped_lock lock(m_mutex);
        m_mailbox.append(std::move(message));
        schedule = !std::exchange(m_drainPosted, true);
    }
    if (schedule) {
        // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
        QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    }
}

void ConverterService::drain()
{
    QList<Message> messages;
    {
        const std::scoped_lock lock(m_mutex);
        messages.swap(m_mailbox);
        m_drainPosted = false;
    }
    bool changed = false;
    for (const Message &message : std::as_const(messages)) {
        const auto found = m_entries.find(message.id);
        if (found == m_entries.end()) {
            continue; // removed meanwhile
        }
        Entry &entry = *found;
        switch (message.kind) {
        case Message::Kind::Read:
            entry.source = message.source;
            if (!message.source->error.isEmpty()) {
                entry.item.state = State::Failed;
                entry.item.message = tr("Selecta can't read this file: %1").arg(message.source->error);
            } else {
                entry.item.state = State::Ready;
                entry.item.quality = convert::describeSource(message.source->quality);
                entry.item.added = message.source->added;
                entry.item.cover = message.source->cover;
                advise(entry);
            }
            changed = true;
            break;
        case Message::Kind::Progress:
            if (entry.item.state == State::Converting) {
                entry.item.progress = std::clamp(message.progress, 0.0, 1.0);
                changed = true;
            }
            break;
        case Message::Kind::Converted:
            converted(message);
            changed = true;
            break;
        }
    }
    if (changed) {
        publish();
    }
}

void ConverterService::converted(const Message &message)
{
    Entry &entry = m_entries[message.id];
    if (entry.item.state != State::Converting) {
        return; // cancelled meanwhile: the worker cleaned up
    }
    entry.cancel.reset();
    entry.item.progress = 1.0;
    if (message.ok) {
        entry.item.state = State::Done;
        entry.item.outputPath = message.outputPath;
        ++m_runDone;
        qCInfo(lcConverter).noquote() << "converted" << entry.item.path << "→" << message.outputPath;
    } else {
        entry.item.state = State::Failed;
        entry.item.message = message.error;
        ++m_runFailed;
        qCWarning(lcConverter).noquote() << "couldn't convert" << entry.item.path << ':' << message.error;
    }
    const bool running =
        std::ranges::any_of(m_entries, [](const Entry &other) { return other.item.state == State::Converting; });
    if (!running) {
        Q_EMIT finished(m_runDone, m_runFailed);
    }
}

void ConverterService::publish()
{
    Q_EMIT itemsChanged(items());
}

QList<ConverterItem> ConverterService::items() const
{
    QList<ConverterItem> items;
    items.reserve(m_order.size());
    for (const qint64 id : m_order) {
        if (const auto found = m_entries.constFind(id); found != m_entries.cend()) {
            items.append(found->item);
        }
    }
    return items;
}

void ConverterService::waitForIdle()
{
    for (int round = 0; round < 100; ++round) {
        m_pool.waitForDone();
        bool pending = false;
        {
            const std::scoped_lock lock(m_mutex);
            pending = !m_mailbox.isEmpty();
        }
        if (!pending) {
            return;
        }
        drain();
    }
}

} // namespace selecta::core
