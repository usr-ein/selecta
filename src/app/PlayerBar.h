// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace selecta::app {

class PreviewController;
class WaveformView;

// The persistent player at the bottom of the window (PRD 8.2, PREV-3): what is playing, the transport, the volume and
// the always-dark waveform screen (UI-3), and, for a peer's complete file, what its spectrum check found (PREV-9). It
// shows the PreviewController's state and sends it the user's commands. Main thread.
class PlayerBar : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerBar(PreviewController &preview, QWidget *parent = nullptr);

    // A one-off line in place of the subtitle, e.g. why a preview couldn't start, or what K or D did to the one
    // playing. Cleared by the next track, and when playing starts.
    void showMessage(const QString &message);

    // A button for one of the window's preview actions (Discard…), after the time as the mock has it; named
    // `objectName`. It follows the action: its icon, tooltip and whether it is enabled.
    void addPreviewAction(QAction *action, const QString &objectName);

    double volume() const;
    void setVolume(double volume); // 0…1

Q_SIGNALS:
    void volumeChanged(double volume);

private:
    void refresh();
    void refreshQuality();
    void tick();
    void trackChanged();

    PreviewController &m_preview;
    QLabel *m_title;
    QLabel *m_subtitle;
    QLabel *m_quality; // the spectrum check's badge: "⚠ Likely from a ≈ 192 kbps MP3"; hidden when it found nothing
    QToolButton *m_back;
    QToolButton *m_play;
    QToolButton *m_forward;
    QLabel *m_time;
    QHBoxLayout *m_actions; // the preview actions' buttons
    QSlider *m_volume;
    WaveformView *m_waveform;
    QTimer *m_ticker; // moves the playhead while playing
    QString m_message;
};

} // namespace selecta::app
