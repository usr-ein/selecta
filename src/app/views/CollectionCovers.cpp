// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/CollectionCovers.h"

#include "convert/ArtworkFinder.h"
#include "convert/TagReader.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QImageReader>
#include <QPromise>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kMaxSourcePixels = 8000; // a side larger than this is never decoded

// A picture's bytes (untrusted), decoded and cut to a square at most `side` pixels a side; null when they aren't a
// picture of a sane size.
QImage decode(QByteArray data, int side)
{
    if (data.isEmpty() || data.size() > CollectionCovers::kMaxPictureBytes) {
        return {};
    }
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > kMaxSourcePixels || size.height() > kMaxSourcePixels) {
        return {};
    }
    // The shorter side at `side`: the tile shows the middle of a cover that isn't square.
    if (size.width() > side && size.height() > side) {
        reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatioByExpanding));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    const int square = std::min(image.width(), image.height());
    if (image.width() != image.height()) {
        image = image.copy((image.width() - square) / 2, (image.height() - square) / 2, square, square);
    }
    return image;
}

// The album folder's own image, chosen as the converter chooses one (CONV-5): cover.jpg, folder.png…
QImage folderCover(const QString &folder, int side)
{
    const QDir dir(folder);
    const QFileInfoList images =
        dir.entryInfoList({u"*.jpg"_s, u"*.jpeg"_s, u"*.png"_s}, QDir::Files | QDir::Readable, QDir::Name);
    QList<convert::CoverCandidate> candidates;
    candidates.reserve(images.size());
    for (const QFileInfo &image : images) {
        candidates.append({.relativePath = image.fileName(), .size = {}});
    }
    const std::optional<convert::CoverChoice> choice = convert::chooseFolderCover(candidates);
    if (!choice || choice->index < 0 || choice->index >= images.size()) {
        return {};
    }
    QFile file(images.at(choice->index).filePath());
    if (file.size() > CollectionCovers::kMaxPictureBytes || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return decode(file.read(CollectionCovers::kMaxPictureBytes), side);
}

// The track's embedded front cover (else its first picture).
QImage embeddedCover(const QString &path, int side)
{
    convert::TagReadOptions options;
    options.maxPictureBytes = CollectionCovers::kMaxPictureBytes;
    options.maxPictures = 4;
    options.maxValueChars = 1024; // the pictures are what is wanted here
    options.maxLyricsChars = 0;
    const convert::Result<convert::MetadataModel> tags = convert::readTags(path, options);
    if (!tags.ok()) {
        return {};
    }
    // Many taggers file the cover as "other": the first picture then.
    std::optional<convert::Picture> cover = tags.value().frontCover();
    if (!cover && !tags.value().pictures().isEmpty()) {
        cover = tags.value().pictures().constFirst();
    }
    return cover ? decode(cover->data, side) : QImage();
}

} // namespace

CollectionCovers::CollectionCovers(QObject *parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(kWorkers);
    m_pool.setObjectName(u"selecta-covers"_s);
    m_cache.setMaxCost(static_cast<qsizetype>(kCacheBytes));
}

CollectionCovers::~CollectionCovers()
{
    m_pool.waitForDone(); // what comes back after that is dropped: the continuations need this object
}

std::optional<QPixmap> CollectionCovers::cover(const QString &key) const
{
    if (const QPixmap *pixmap = m_cache.object(key)) {
        return *pixmap;
    }
    return std::nullopt;
}

void CollectionCovers::request(const QString &key, const Source &source)
{
    if (key.isEmpty() || m_cache.contains(key) || m_asked.contains(key)) {
        return;
    }
    m_asked.insert(key);
    m_waiting.append(Request{.key = key, .source = source});
    while (m_waiting.size() > kMaxWaiting) {
        m_asked.remove(m_waiting.takeFirst().key); // scrolled past long ago: asked again if it shows again
    }
    startNext();
}

void CollectionCovers::clear()
{
    ++m_generation;
    m_cache.clear();
    m_waiting.clear();
    m_asked.clear();
}

void CollectionCovers::startNext()
{
    while (m_running < kWorkers && !m_waiting.isEmpty()) {
        const Request next = m_waiting.takeLast(); // the newest: the tiles on screen now
        ++m_running;
        const auto promise = std::make_shared<QPromise<QImage>>();
        QFuture<QImage> future = promise->future();
        promise->start();
        m_pool.start([promise, source = next.source] {
            promise->addResult(read(source));
            promise->finish();
        });
        future.then(this, [this, key = next.key, generation = m_generation](const QImage &image) {
            finished(key, generation, image);
        });
    }
}

void CollectionCovers::finished(const QString &key, quint64 generation, const QImage &image)
{
    --m_running;
    if (generation == m_generation) {
        m_asked.remove(key);
        // "No cover" is remembered at a nominal cost, so it isn't read again and again.
        const qsizetype cost = image.isNull() ? 1 : static_cast<qsizetype>(image.sizeInBytes());
        m_cache.insert(key, new QPixmap(image.isNull() ? QPixmap() : QPixmap::fromImage(image)), cost);
        Q_EMIT ready(key);
    }
    startNext();
}

QImage CollectionCovers::read(const Source &source, int side)
{
    if (!source.folder.isEmpty()) {
        if (QImage cover = folderCover(source.folder, side); !cover.isNull()) {
            return cover;
        }
    }
    return source.file.isEmpty() ? QImage() : embeddedCover(source.file, side);
}

} // namespace selecta::app
