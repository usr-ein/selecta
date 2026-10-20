// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace selecta::app {

class PreviewController;
class WaveformView;

// The persistent player at the bottom of the window (PRD 8.2, PREV-3): what is playing, the transport, the volume and
// the always-dark waveform screen (UI-3). It shows the PreviewController's state and sends it the user's commands.
// Main thread.
class PlayerBar : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerBar(PreviewController &preview, QWidget *parent = nullptr);

    // A one-off line in place of the subtitle, e.g. why a preview couldn't start. Cleared by the next track.
    void showMessage(const QString &message);

    double volume() const;
    void setVolume(double volume); // 0…1

Q_SIGNALS:
    void volumeChanged(double volume);

private:
    void refresh();
    void tick();
    void trackChanged();

    PreviewController &m_preview;
    QLabel *m_title;
    QLabel *m_subtitle;
    QToolButton *m_back;
    QToolButton *m_play;
    QToolButton *m_forward;
    QLabel *m_time;
    QSlider *m_volume;
    WaveformView *m_waveform;
    QTimer *m_ticker; // moves the playhead while playing
    QString m_message;
};

} // namespace selecta::app
