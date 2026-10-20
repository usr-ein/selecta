// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Waveform.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <array>
#include <optional>

namespace selecta::app {

// The waveform screen of the player (PRD PREV-3, UI-3), dark in both appearances like a CDJ. On top, the zoomed view
// scrolls under a fixed centre playhead; below, the overview shows the whole track, the part already played and the
// part not downloaded yet. Colours and layering follow rekordbox's 3-band waveform (references/
// audio-preview-and-waveform.md §2): mirrored detail with a brown low∩mid overlap and translucent white highs in
// front; the overview stacked from the bottom.
//
// It only draws what it is given. Clicking or dragging the overview, or dragging the zoomed view, asks for a seek
// when the button is released. Main thread.
class WaveformView : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformView(QWidget *parent = nullptr);

    // A partial waveform (still analysing or downloading) or a complete one; a null waveform clears the bands.
    void setWaveform(const audio::Waveform &waveform);
    // The track's length; known from the player before the analysis is done. 0 while unknown.
    void setDuration(double seconds);
    void setPosition(double seconds);
    // Seconds, from the start, that have downloaded; the rest of the track is hatched. Negative: all of it.
    void setBuffered(double seconds);
    // Shown in the zoomed view when there is nothing to draw.
    void setMessage(const QString &message);
    void clear();

    double position() const { return m_position; }
    // The span the zoomed view shows.
    static constexpr double kZoomSeconds = 8.0;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void seekRequested(double seconds);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    // Band levels for one pixel column, normalised for drawing (0…1); low < 0 means "no data here".
    struct Levels
    {
        float low = -1.0F;
        float mid = 0.0F;
        float high = 0.0F;
    };
    enum class Drag { None, Overview, Detail };

    QRect detailRect() const;
    QRect overviewRect() const;
    double duration() const;
    double shownPosition() const;
    double timeAtOverview(int x) const;
    Levels levelsBetween(double from, double to) const;
    Levels normalised(std::array<int, audio::kBandCount> peaks) const;
    void updateGains();
    QImage renderDetail(QSize pixels, double from, double to) const;
    QImage renderOverview(QSize pixels) const;
    void paintDetailOverlay(QPainter &painter, QRect rect, double from, double to) const;
    void paintOverviewOverlay(QPainter &painter, QRect rect) const;

    audio::Waveform m_waveform;
    std::array<float, audio::kBandCount> m_gain{1.0F, 1.0F, 1.0F}; // per-band normalisation for this track
    double m_duration = 0.0;
    double m_position = 0.0;
    double m_buffered = -1.0;
    QString m_message;
    QImage m_overviewCache; // rebuilt when the waveform, length or size changes
    Drag m_drag = Drag::None;
    int m_dragStartX = 0;
    double m_dragStartPosition = 0.0;
    std::optional<double> m_dragPosition; // shown instead of the playhead while dragging
    std::optional<int> m_hoverX;          // over the overview
};

} // namespace selecta::app
