// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Spectrogram.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <optional>

namespace selecta::app {

// A file's spectrogram (PRD PREV-9): time from left to right, frequency from 0 Hz at the bottom to Nyquist at the top,
// each cell's level on a perceptual colour scale (inferno: black, purple, red, orange, pale yellow; -120 dB to 0 dB),
// on a dark ground in both appearances like the waveform. Frequencies are ticked every 5 kHz (10, 20 or 50 kHz when the
// view is short or the rate high). The spectral cut-off the quality check measured is a dashed line with its frequency
// written beside it, so the mark doesn't rely on colour (UI-4). Hovering shows the time and frequency under the
// pointer.
//
// It only draws what it is given. Main thread.
class SpectrogramView : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget *parent = nullptr);

    // A null spectrogram clears the grid.
    void setSpectrogram(const audio::Spectrogram &spectrogram);
    const audio::Spectrogram &spectrogram() const { return m_spectrogram; }
    // The measured cut-off in Hz; nullopt (or anything outside 0 Hz…Nyquist) draws no line.
    void setCutoff(std::optional<int> hz);
    std::optional<int> cutoff() const { return m_cutoffHz; }
    // Shown when there is no grid to draw: "Loading…", why it's missing.
    void setMessage(const QString &message);
    QString message() const { return m_message; }
    void clear();

    // The colour scale, from a level byte (Spectrogram::levelForDb()).
    static QRgb colorForLevel(quint8 level);
    // Where the pointer is, for the hover text: e.g. "1:23 · 16.4 kHz"; empty outside the grid.
    QString textAt(QPoint point) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect plotRect() const;
    void refreshDescription();

    audio::Spectrogram m_spectrogram;
    QImage m_grid; // one pixel per cell, 0 Hz at the bottom row; scaled when painted
    std::optional<int> m_cutoffHz;
    QString m_message;
};

} // namespace selecta::app
