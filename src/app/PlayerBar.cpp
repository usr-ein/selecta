// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/PlayerBar.h"

#include "app/Format.h"
#include "app/Icons.h"
#include "app/Preview.h"
#include "app/WaveformView.h"

#include <QAction>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using State = playback::Player::State;

constexpr int kTickMs = 16; // playhead updates while playing: one per frame at 60 Hz
constexpr double kSkipSeconds = 5.0;

// A transport control as macOS players draw it: the symbol alone, in the label colour, with no bezel around it. It dims
// while pressed, and when disabled.
class GlyphButton : public QToolButton
{
    Q_OBJECT

public:
    explicit GlyphButton(QWidget *parent)
        : QToolButton(parent)
    {
        setAutoRaise(true);
        setCursor(Qt::ArrowCursor);
    }

    QSize sizeHint() const override { return iconSize() + QSize(kGlyphPadding, kGlyphPadding); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QColor color = palette().color(QPalette::Active, QPalette::WindowText);
        if (!isEnabled()) {
            color.setAlphaF(kDisabledAlpha);
        } else if (isDown()) {
            color.setAlphaF(kPressedAlpha);
        }
        const QPixmap glyph = icons::tinted(icon(), iconSize(), devicePixelRatioF(), color);
        if (glyph.isNull()) {
            return;
        }
        const QSizeF size = glyph.deviceIndependentSize();
        QPainter painter(this);
        painter.drawPixmap(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), glyph);
    }

private:
    static constexpr int kGlyphPadding = 8;
    static constexpr float kPressedAlpha = 0.55F;
    static constexpr float kDisabledAlpha = 0.3F;
};

QToolButton *transportButton(const QString &name, const QString &symbol, const QString &fallback,
                             const QString &toolTip, int size, QWidget *parent)
{
    auto *button = new GlyphButton(parent);
    button->setObjectName(name);
    button->setIcon(icons::symbol(symbol, fallback));
    button->setIconSize(QSize(size, size));
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    button->setFocusPolicy(Qt::NoFocus); // Space belongs to the window's play/pause, not to a focused button
    return button;
}

} // namespace

PlayerBar::PlayerBar(PreviewController &preview, QWidget *parent)
    : QWidget(parent)
    , m_preview(preview)
    , m_title(new QLabel(this))
    , m_subtitle(new QLabel(this))
    , m_quality(new QLabel(this))
    , m_back(transportButton(u"player.back"_s, u"gobackward.5"_s, u"media-seek-backward"_s, tr("Back 5 seconds"), 18,
                             this))
    , m_play(transportButton(u"player.play"_s, u"play.fill"_s, u"media-playback-start"_s, tr("Play (Space)"), 22, this))
    , m_forward(transportButton(u"player.forward"_s, u"goforward.5"_s, u"media-seek-forward"_s, tr("Forward 5 seconds"),
                                18, this))
    , m_left(new QWidget(this))
    , m_right(new QWidget(this))
    , m_time(new QLabel(this))
    , m_actions(new QHBoxLayout)
    , m_volume(new QSlider(Qt::Horizontal, this))
    , m_speaker(new QLabel(this))
    , m_waveform(new WaveformView(this))
    , m_ticker(new QTimer(this))
{
    setObjectName(u"player"_s);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);

    m_title->setObjectName(u"player.title"_s);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_title->setTextFormat(Qt::PlainText);
    m_subtitle->setObjectName(u"player.subtitle"_s);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    m_subtitle->setTextFormat(Qt::PlainText);
    for (QLabel *label : {m_title, m_subtitle}) {
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long names elide, never widen the bar
    }
    m_quality->setObjectName(u"player.quality"_s);
    m_quality->setTextFormat(Qt::PlainText);
    m_quality->hide();

    m_time->setObjectName(u"player.time"_s);
    QFont timeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    timeFont.setPointSizeF(m_time->font().pointSizeF());
    m_time->setFont(timeFont);
    m_time->setForegroundRole(QPalette::PlaceholderText);
    m_time->setMinimumWidth(QFontMetrics(timeFont).horizontalAdvance(u"88:88 / 88:88"_s)); // steady while it ticks

    m_volume->setObjectName(u"player.volume"_s);
    m_volume->setRange(0, 100);
    m_volume->setValue(80);
    m_volume->setFixedWidth(96);
    m_volume->setFocusPolicy(Qt::NoFocus);
    m_volume->setToolTip(tr("Preview volume"));
    m_volume->setAccessibleName(tr("Preview volume"));
    m_speaker->setObjectName(u"player.speaker"_s);
    tintSpeaker();

    auto *texts = new QVBoxLayout;
    texts->setSpacing(0);
    texts->addWidget(m_title);
    texts->addWidget(m_subtitle);

    // Three parts, as macOS players have them: what is playing on the left, the transport in the middle of the bar,
    // the time, the actions and the volume on the right. The two sides share what the transport leaves equally, so
    // the transport stays centred; each is as wide as the right one needs at least (syncSides).
    m_left->setObjectName(u"player.left"_s);
    auto *left = new QHBoxLayout(m_left);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(6);
    left->addLayout(texts, 1);
    left->addWidget(m_quality);

    auto *transport = new QHBoxLayout;
    transport->setSpacing(4);
    transport->addWidget(m_back);
    transport->addWidget(m_play);
    transport->addWidget(m_forward);

    m_right->setObjectName(u"player.right"_s);
    auto *right = new QHBoxLayout(m_right);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(6);
    right->addStretch(1);
    right->addWidget(m_time);
    m_actions->setSpacing(2);
    right->addLayout(m_actions);
    right->addSpacing(12);
    right->addWidget(m_speaker);
    right->addWidget(m_volume);

    for (QWidget *side : {m_left, m_right}) {
        side->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    auto *row = new QHBoxLayout;
    row->setSpacing(16);
    row->addWidget(m_left, 1);
    row->addLayout(transport);
    row->addWidget(m_right, 1);

    m_waveform->setMinimumHeight(112);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 12);
    layout->setSpacing(8);
    layout->addLayout(row);
    layout->addWidget(m_waveform, 1);

    playback::Player &player = m_preview.player();
    connect(m_play, &QToolButton::clicked, &m_preview, &PreviewController::toggle);
    connect(m_back, &QToolButton::clicked, this, [this] { m_preview.player().skip(-kSkipSeconds); });
    connect(m_forward, &QToolButton::clicked, this, [this] { m_preview.player().skip(kSkipSeconds); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_preview.player().setVolume(value / 100.0);
        Q_EMIT volumeChanged(value / 100.0);
    });
    player.setVolume(m_volume->value() / 100.0);
    connect(m_waveform, &WaveformView::seekRequested, &player, &playback::Player::seek);

    connect(&player, &playback::Player::stateChanged, this, [this](State state) {
        if (state == State::Playing) {
            m_message.clear(); // "Asking vinyl_ghost…" and the like are over once it plays
        }
        refresh();
    });
    connect(&player, &playback::Player::detailChanged, this, &PlayerBar::refresh);
    connect(&player, &playback::Player::trackChanged, this, &PlayerBar::trackChanged);
    connect(&player, &playback::Player::durationChanged, m_waveform, &WaveformView::setDuration);
    connect(&player, &playback::Player::durationChanged, this, &PlayerBar::tick);
    connect(&player, &playback::Player::bufferedChanged, this, [this](double seconds) {
        const std::shared_ptr<audio::ByteSource> source = m_preview.player().source();
        m_waveform->setBuffered(source && source->snapshot().isComplete() ? -1.0 : seconds);
    });
    connect(&m_preview, &PreviewController::waveformChanged, this,
            [this] { m_waveform->setWaveform(m_preview.waveform()); });
    connect(&m_preview, &PreviewController::qualityChanged, this, &PlayerBar::refreshQuality);
    m_ticker->setInterval(kTickMs);
    m_ticker->setTimerType(Qt::PreciseTimer);
    connect(m_ticker, &QTimer::timeout, this, &PlayerBar::tick);
    refresh();
    syncSides();
}

void PlayerBar::showMessage(const QString &message)
{
    m_message = message;
    refresh();
}

void PlayerBar::addPreviewAction(QAction *action, const QString &objectName)
{
    auto *button = new GlyphButton(m_right);
    button->setObjectName(objectName);
    button->setDefaultAction(action);
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAccessibleName(action->text());
    button->setFocusPolicy(Qt::NoFocus); // as the transport: keys belong to the window's shortcuts
    m_actions->addWidget(button);
    syncSides();
}

void PlayerBar::syncSides()
{
    // Both sides as wide as the right one's content needs: equal minimums and equal stretch keep the transport in the
    // middle; past that minimum the window is too narrow to centre it, and the left side's names elide first.
    const int needed = m_right->layout()->sizeHint().width();
    m_left->setMinimumWidth(needed);
    m_right->setMinimumWidth(needed);
}

void PlayerBar::tintSpeaker()
{
    const QIcon speaker = icons::symbol(u"speaker.wave.2.fill"_s, u"audio-volume-medium"_s);
    m_speaker->setPixmap(icons::tinted(speaker, QSize(14, 14), devicePixelRatioF(),
                                       palette().color(QPalette::Active, QPalette::WindowText)));
}

void PlayerBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        tintSpeaker(); // light and dark
    }
}

double PlayerBar::volume() const
{
    return m_volume->value() / 100.0;
}

void PlayerBar::setVolume(double volume)
{
    m_volume->setValue(static_cast<int>(std::lround(std::clamp(volume, 0.0, 1.0) * 100.0)));
}

void PlayerBar::trackChanged()
{
    m_message.clear();
    const playback::Player &player = m_preview.player();
    if (!player.isActive() && player.state() != State::Ended) {
        m_waveform->clear();
    } else {
        m_waveform->setDuration(player.duration());
        m_waveform->setPosition(0.0);
        m_waveform->setBuffered(-1.0);
    }
    refresh();
}

void PlayerBar::refresh()
{
    const playback::Player &player = m_preview.player();
    const playback::PlayerTrack &track = player.track();
    const State state = player.state();
    const bool loaded = player.isActive() || state == State::Ended;

    if (!loaded && state != State::Failed) {
        m_title->setText(tr("Nothing playing"));
        m_subtitle->setText(m_message.isEmpty() ? tr("Select a track and press Space to preview it.") : m_message);
        m_waveform->setMessage(tr("Select a track and press Space to preview it"));
    } else {
        m_title->setText(track.title.isEmpty() ? tr("Untitled") : track.title);
        QStringList parts;
        if (!track.artist.isEmpty()) {
            parts.append(track.artist);
        }
        if (!track.source.isEmpty()) {
            parts.append(track.source);
        }
        switch (state) {
        case State::Loading:
            parts.append(player.detail().isEmpty() ? tr("Loading…") : player.detail());
            break;
        case State::Paused:
            parts.append(tr("Paused"));
            break;
        case State::Buffering:
            parts.append(tr("Waiting for the download…"));
            break;
        case State::Ended:
            parts.append(tr("Ended"));
            break;
        case State::Failed:
            parts = QStringList{tr("Can't play: %1").arg(player.error())};
            break;
        case State::Playing:
        case State::Idle:
            break;
        }
        m_subtitle->setText(m_message.isEmpty() ? parts.join(u" · "_s) : m_message);
        m_waveform->setMessage({});
        m_waveform->setSource(track.source); // for VoiceOver, with the position
    }

    const bool running = state == State::Playing || state == State::Buffering || state == State::Loading;
    m_play->setIcon(running ? icons::symbol(u"pause.fill"_s, u"media-playback-pause"_s)
                            : icons::symbol(u"play.fill"_s, u"media-playback-start"_s));
    m_play->setToolTip(running ? tr("Pause (Space)") : tr("Play (Space)"));
    m_play->setAccessibleName(running ? tr("Pause") : tr("Play"));
    m_play->setEnabled(loaded);
    m_back->setEnabled(loaded);
    m_forward->setEnabled(loaded);
    if (running) {
        m_ticker->start();
    } else {
        m_ticker->stop();
    }
    tick();
}

void PlayerBar::refreshQuality()
{
    // A symbol and words (UI-4), worded "likely" (PREV-9); the details in the tooltip.
    const std::optional<audio::QualityReport> &report = m_preview.quality();
    if (!report || report->findings.isEmpty()) {
        m_quality->hide();
        m_quality->clear();
        m_quality->setToolTip({});
        return;
    }
    m_quality->setText(tr("⚠ %1").arg(audio::findingBadge(report->findings.constFirst(), report->likelySourceKbps)));
    QStringList details;
    for (const audio::QualityFinding finding : report->findings) {
        details.append(audio::describeFinding(finding, *report) + u'.');
    }
    m_quality->setToolTip(details.join(u'\n'));
    m_quality->setAccessibleName(m_quality->text());
    m_quality->show();
}

void PlayerBar::tick()
{
    const playback::Player &player = m_preview.player();
    const double position = player.position();
    m_waveform->setPosition(position);
    const double duration = player.duration();
    if (!player.isActive() && player.state() != State::Ended) {
        m_time->clear();
        return;
    }
    const QString length = duration > 0.0 ? format::clock(duration) : u"–:––"_s;
    m_time->setText(format::clock(position) + u" / "_s + length);
}

} // namespace selecta::app

#include "PlayerBar.moc"
