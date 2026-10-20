// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Preview.h"

#include "audio/WaveformAnalyzer.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFuture>
#include <QLoggingCategory>
#include <QPromise>
#include <QThreadPool>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

Q_LOGGING_CATEGORY(lcPreview, "selecta.app.preview")

const QStringList &audioSuffixes()
{
    static const QStringList suffixes{u"mp3"_s,  u"flac"_s, u"wav"_s, u"aif"_s,
                                      u"aiff"_s, u"m4a"_s,  u"ogg"_s, u"opus"_s};
    return suffixes;
}

// An album folder's first track, in the order Finder shows them.
QString firstTrackIn(const QString &folder)
{
    QStringList names = QDir(folder).entryList(QDir::Files | QDir::Readable);
    names.removeIf([](const QString &name) {
        return name.startsWith(u'.') || !audioSuffixes().contains(QFileInfo(name).suffix().toLower());
    });
    if (names.isEmpty()) {
        return {};
    }
    QCollator collator;
    collator.setNumericMode(true);
    std::ranges::sort(names, collator);
    return QDir(folder).filePath(names.constFirst());
}

// The spectrum check of a complete preview, from the cache when it ran before. Blocking: a worker's.
std::optional<audio::QualityReport> checkQuality(const std::shared_ptr<audio::ByteSource> &source,
                                                 audio::AnalysisCache *cache, const QString &identity,
                                                 const audio::CancellationFlag *cancel)
{
    const audio::CacheKey key = audio::CacheKey::forName(identity);
    if (cache != nullptr) {
        if (std::optional<audio::QualityReport> cached = cache->loadQuality(key)) {
            return cached;
        }
    }
    audio::Result<audio::QualityReport> report = audio::analyzeQuality(source, {}, cancel);
    if (!report) {
        if (report.error().code != audio::ErrorCode::Cancelled) {
            qCInfo(lcPreview).noquote() << "no spectrum check:" << report.error().message;
        }
        return std::nullopt;
    }
    if (cache != nullptr) {
        if (const audio::Result<void> kept = cache->storeQuality(key, report.value()); !kept) {
            qCInfo(lcPreview).noquote() << "spectrum check not cached:" << kept.error().message;
        }
    }
    return report.takeValue();
}

} // namespace

LocalPreview previewForPath(const QString &path, playback::PlayerTrack track)
{
    const QFileInfo info(path);
    QString file = path;
    if (info.isDir()) {
        file = firstTrackIn(path);
        if (file.isEmpty()) {
            return {std::nullopt, PreviewController::tr("There is no audio file in %1.").arg(info.fileName())};
        }
    } else if (!info.exists()) {
        return {std::nullopt, PreviewController::tr("%1 isn't there any more.").arg(info.fileName())};
    }
    audio::Result<std::shared_ptr<audio::FileSource>> source = audio::FileSource::open(file);
    if (!source) {
        return {
            std::nullopt,
            PreviewController::tr("Selecta can't read %1: %2").arg(QFileInfo(file).fileName(), source.error().message)};
    }
    const QFileInfo fileInfo(file);
    PreviewRequest request;
    request.source = source.takeValue();
    request.track = std::move(track);
    request.identity = u"file|%1|%2|%3"_s.arg(fileInfo.absoluteFilePath())
                           .arg(fileInfo.size())
                           .arg(fileInfo.lastModified().toMSecsSinceEpoch());
    return {std::move(request), {}};
}

PreviewController::PreviewController(std::unique_ptr<playback::AudioOutput> output,
                                     std::shared_ptr<audio::AnalysisCache> cache, QObject *parent)
    : QObject(parent)
    , m_player(std::move(output))
    , m_cache(std::move(cache))
{}

PreviewController::~PreviewController()
{
    cancelAnalysis();
}

void PreviewController::preview(const PreviewRequest &request)
{
    if (!request.source) {
        return;
    }
    cancelAnalysis();
    m_identity = request.identity;
    m_source = request.source;
    m_columns.clear();
    m_waveform = {};
    Q_EMIT waveformChanged();
    if (m_quality) {
        m_quality.reset();
        Q_EMIT qualityChanged();
    }
    m_player.play(request.source, request.track);
    startAnalysis(request);
}

void PreviewController::toggle()
{
    m_player.toggle();
}

void PreviewController::stop()
{
    cancelAnalysis();
    m_player.stop();
    m_identity.clear();
    m_source.reset();
    m_columns.clear();
    m_waveform = {};
    Q_EMIT waveformChanged();
    if (m_quality) {
        m_quality.reset();
        Q_EMIT qualityChanged();
    }
}

void PreviewController::startAnalysis(const PreviewRequest &request)
{
    m_job = new audio::WaveformJob(request.source, {}, this);
    if (m_cache && !request.identity.isEmpty()) {
        m_job->setCache(m_cache, audio::CacheKey::forName(request.identity));
    }
    connect(m_job, &audio::WaveformJob::columnsAdded, this,
            [this](const QList<audio::WaveformColumn> &columns, qint64 firstColumn, qint64 expectedColumns) {
                Q_UNUSED(expectedColumns)
                if (firstColumn != m_columns.size()) {
                    qCWarning(lcPreview) << "waveform columns out of order:" << firstColumn << "after"
                                         << m_columns.size();
                    return;
                }
                m_columns.append(columns);
                // One frame per column: a partial waveform only needs its columns, at 150 per second.
                m_waveform = audio::Waveform::fromDetail(audio::kColumnsPerSecond, m_columns.size(), m_columns, false);
                Q_EMIT waveformChanged();
            });
    connect(m_job, &audio::WaveformJob::finished, this, [this](const audio::Waveform &waveform) {
        m_waveform = waveform;
        m_columns.clear();
        Q_EMIT waveformChanged();
        // The whole file is there now: a peer's gets the spectrum check (PREV-9).
        if (m_identity.startsWith(u"peer|"_s)) {
            startQualityCheck();
        }
    });
    connect(m_job, &audio::WaveformJob::failed, this, [](const audio::Error &error) {
        qCInfo(lcPreview).noquote() << "no waveform:" << error.message; // the preview still plays
    });
    m_job->start();
}

void PreviewController::cancelAnalysis()
{
    if (m_job) {
        m_job->cancel(); // no signal after this returns
        m_job->deleteLater();
        m_job = nullptr;
    }
    ++m_generation; // a check in flight is for the previous preview: its result is dropped
    if (m_qualityCancel) {
        m_qualityCancel->cancel();
        m_qualityCancel.reset();
    }
}

void PreviewController::startQualityCheck()
{
    if (!m_source) {
        return;
    }
    auto cancel = std::make_shared<audio::CancellationFlag>();
    m_qualityCancel = cancel;
    const auto promise = std::make_shared<QPromise<std::optional<audio::QualityReport>>>();
    QFuture<std::optional<audio::QualityReport>> future = promise->future();
    promise->start();
    QThreadPool::globalInstance()->start([promise, source = m_source, cache = m_cache, identity = m_identity, cancel] {
        promise->addResult(checkQuality(source, cache.get(), identity, cancel.get()));
        promise->finish();
    });
    future.then(this, [this, generation = m_generation](const std::optional<audio::QualityReport> &report) {
        if (generation != m_generation || !report) {
            return;
        }
        m_quality = report;
        m_qualityCancel.reset();
        Q_EMIT qualityChanged();
    });
}

} // namespace selecta::app
