// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Preview.h"

#include "audio/WaveformAnalyzer.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

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
    m_columns.clear();
    m_waveform = {};
    Q_EMIT waveformChanged();
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
    m_columns.clear();
    m_waveform = {};
    Q_EMIT waveformChanged();
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
}

} // namespace selecta::app
