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
// when the button is released; with focus (Tab), Left and Right ask for one 5 seconds away. VoiceOver sees a slider
// (UI-5, WaveformAccessible in the .cpp): "1:23 of 6:12", the source and how much has downloaded, and steps it as the
// arrow keys do. Main thread.
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
    // Where the audio comes from ("Preview from vinyl_ghost"), for VoiceOver; it is on screen above the waveform.
    void setSource(const QString &source);
    void clear();

    double position() const { return m_position; }
    // The track's length as far as it is known: the player's, else the complete analysis's; 0 while unknown.
    double duration() const;
    // Asks for a seek `seconds` away from the position, kept within the track; nothing without a length.
    void step(double seconds);
    // What VoiceOver reads as the value, "1:23 of 6:12", and as the description: the source, the part downloaded
    // and the keys.
    QString positionText() const;
    QString detailText() const;

    // The span the zoomed view shows.
    static constexpr double kZoomSeconds = 8.0;
    // One press of Left or Right, as the player bar's 5-second buttons.
    static constexpr double kStepSeconds = 5.0;

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
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

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
    void requestSeek(double seconds);
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
    QString m_source;
    QImage m_overviewCache; // rebuilt when the waveform, length or size changes
    Drag m_drag = Drag::None;
    int m_dragStartX = 0;
    double m_dragStartPosition = 0.0;
    std::optional<double> m_dragPosition; // shown instead of the playhead while dragging
    std::optional<int> m_hoverX;          // over the overview
};

} // namespace selecta::app
