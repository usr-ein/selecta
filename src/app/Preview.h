// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/AnalysisCache.h"
#include "audio/ByteSource.h"
#include "audio/QualityCheck.h"
#include "audio/Waveform.h"
#include "playback/Player.h"

#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace selecta::audio {
class WaveformJob;
}

namespace selecta::app {

// One thing to preview: where its bytes come from, what the player bar calls it, and a stable name for the analysis
// cache.
struct PreviewRequest
{
    std::shared_ptr<audio::ByteSource> source;
    playback::PlayerTrack track;
    // Names the audio for the analysis cache and for "is this what's playing?", e.g. "file|<path>|<size>|<mtime>".
    // Empty: the waveform is analysed but not cached.
    QString identity;
};

// A file in the Inbox or the collection as a preview: the file itself, or an album folder's first track. Fails with
// a reason the player bar can show when there is nothing playable there.
struct LocalPreview
{
    std::optional<PreviewRequest> request;
    QString error;
};
LocalPreview previewForPath(const QString &path, playback::PlayerTrack track);

// The preview (PRD PREV-1…3): plays a source and analyses its waveform side by side, so the waveform fills in while
// the track plays, or while it downloads. A peer's file, once complete, also gets the spectrum check (PREV-9). Owns
// the player. Main thread; the analyses run on the global thread pool.
class PreviewController : public QObject
{
    Q_OBJECT

public:
    PreviewController(std::unique_ptr<playback::AudioOutput> output, std::shared_ptr<audio::AnalysisCache> cache,
                      QObject *parent = nullptr);
    ~PreviewController() override;
    PreviewController(const PreviewController &) = delete;
    PreviewController &operator=(const PreviewController &) = delete;
    PreviewController(PreviewController &&) = delete;
    PreviewController &operator=(PreviewController &&) = delete;

    playback::Player &player() { return m_player; }
    const playback::Player &player() const { return m_player; }
    // What has been analysed of the current preview: partial while it downloads, null before the first columns.
    const audio::Waveform &waveform() const { return m_waveform; }
    // What the spectrum check (PREV-9) found about the current preview, once its file is complete: run on a peer's
    // file only (identity "peer|…", the very file a Get would download). YouTube and Deezer previews are lossy by
    // design and claim no quality, so they aren't judged. nullopt until then, for other previews, and when the file
    // can't be analysed. Kept in the analysis cache under the preview's identity.
    const std::optional<audio::QualityReport> &quality() const { return m_quality; }
    // The identity of what is loaded; empty when nothing is.
    QString identity() const { return m_identity; }
    // The waveform cache (Settings shows its size and can empty it); null when there is none.
    audio::AnalysisCache *analysisCache() const { return m_cache.get(); }
    // The same, for owners that outlive this controller or hand it to workers (the Inbox inspector).
    std::shared_ptr<audio::AnalysisCache> sharedAnalysisCache() const { return m_cache; }

    // Plays `request` from the start, replacing the current preview.
    void preview(const PreviewRequest &request);
    // Plays or pauses; after the end, plays again.
    void toggle();
    void stop();

Q_SIGNALS:
    void waveformChanged();
    void qualityChanged();

private:
    void startAnalysis(const PreviewRequest &request);
    void cancelAnalysis();
    void startQualityCheck();

    playback::Player m_player;
    std::shared_ptr<audio::AnalysisCache> m_cache;
    audio::WaveformJob *m_job = nullptr; // child; one per preview
    QList<audio::WaveformColumn> m_columns;
    audio::Waveform m_waveform;
    QString m_identity;
    std::shared_ptr<audio::ByteSource> m_source; // the current preview's
    std::optional<audio::QualityReport> m_quality;
    std::shared_ptr<audio::CancellationFlag> m_qualityCancel; // the check in flight
    quint64 m_generation = 0;                                 // which preview a check in flight belongs to
};

} // namespace selecta::app
