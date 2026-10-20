// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SpectrogramView.h"

#include "app/Format.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr QRgb kBackground = qRgb(0x0b, 0x0c, 0x0f); // the waveform's
// matplotlib's inferno (CC0) at 0, 0.1, …, 1: perceptually uniform, and still ordered in greyscale and for colour-blind
// eyes.
constexpr std::array<QRgb, 11> kInferno = {qRgb(0x00, 0x00, 0x04), qRgb(0x16, 0x0b, 0x39), qRgb(0x42, 0x0a, 0x68),
                                           qRgb(0x6a, 0x17, 0x6e), qRgb(0x93, 0x26, 0x67), qRgb(0xbc, 0x37, 0x54),
                                           qRgb(0xdd, 0x51, 0x3a), qRgb(0xf3, 0x76, 0x1b), qRgb(0xfc, 0xa5, 0x0a),
                                           qRgb(0xf6, 0xd7, 0x46), qRgb(0xfc, 0xff, 0xa4)};
constexpr std::array<int, 4> kTickSteps = {5, 10, 20, 50}; // kHz: the first that leaves room between labels
constexpr qreal kMinTickGap = 22.0;                        // points

std::array<QRgb, 256> makeScale()
{
    std::array<QRgb, 256> scale{};
    for (std::size_t level = 0; level < scale.size(); ++level) {
        const double position = static_cast<double>(level) / 255.0 * static_cast<double>(kInferno.size() - 1);
        const auto low = static_cast<std::size_t>(position);
        const std::size_t high = std::min(low + 1, kInferno.size() - 1);
        const double t = position - static_cast<double>(low);
        const auto mix = [t](int a, int b) { return static_cast<int>(std::lround(a + ((b - a) * t))); };
        scale.at(level) = qRgb(mix(qRed(kInferno.at(low)), qRed(kInferno.at(high))),
                               mix(qGreen(kInferno.at(low)), qGreen(kInferno.at(high))),
                               mix(qBlue(kInferno.at(low)), qBlue(kInferno.at(high))));
    }
    return scale;
}

QString kiloHertz(double hz)
{
    const double rounded = std::round(hz / 100.0) / 10.0;
    return std::fmod(rounded, 1.0) == 0.0 ? u"%1 kHz"_s.arg(static_cast<int>(rounded))
                                          : u"%1 kHz"_s.arg(rounded, 0, 'f', 1);
}

} // namespace

SpectrogramView::SpectrogramView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"spectrogram"_s);
    setMouseTracking(true);
    setAccessibleName(tr("Spectrogram"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    refreshDescription();
}

QRgb SpectrogramView::colorForLevel(quint8 level)
{
    static const std::array<QRgb, 256> scale = makeScale();
    return scale.at(level);
}

void SpectrogramView::setSpectrogram(const audio::Spectrogram &spectrogram)
{
    m_spectrogram = spectrogram;
    m_grid = {};
    if (!spectrogram.isNull() && !spectrogram.isEmpty()) {
        const int columns = spectrogram.columns();
        const int bins = spectrogram.bins();
        QImage grid(columns, bins, QImage::Format_RGB32);
        for (int c = 0; c < columns; ++c) {
            const std::span<const quint8> column = spectrogram.column(c);
            for (int b = 0; b < bins; ++b) {
                reinterpret_cast<QRgb *>(grid.scanLine(bins - 1 - b))[c] = colorForLevel(column[b]);
            }
        }
        m_grid = std::move(grid);
    }
    refreshDescription();
    update();
}

void SpectrogramView::setCutoff(std::optional<int> hz)
{
    m_cutoffHz = hz;
    refreshDescription();
    update();
}

void SpectrogramView::setMessage(const QString &message)
{
    m_message = message;
    refreshDescription();
    update();
}

void SpectrogramView::clear()
{
    m_cutoffHz.reset();
    m_message.clear();
    setSpectrogram({});
}

QSize SpectrogramView::sizeHint() const
{
    return {280, 120};
}

QSize SpectrogramView::minimumSizeHint() const
{
    return {120, 80};
}

QRect SpectrogramView::plotRect() const
{
    return rect();
}

QString SpectrogramView::textAt(QPoint point) const
{
    const QRect plot = plotRect();
    if (m_grid.isNull() || !plot.contains(point) || plot.width() <= 0 || plot.height() <= 0) {
        return {};
    }
    const int column = std::clamp(
        static_cast<int>(static_cast<qint64>(point.x() - plot.left()) * m_spectrogram.columns() / plot.width()), 0,
        m_spectrogram.columns() - 1);
    const double seconds =
        static_cast<double>(m_spectrogram.columnStartFrame(column)) / std::max(1, m_spectrogram.sampleRate());
    const double hz = (plot.bottom() + 1 - point.y()) * m_spectrogram.nyquistHz() / plot.height();
    return u"%1 · %2"_s.arg(format::clock(seconds), kiloHertz(std::clamp(hz, 0.0, m_spectrogram.nyquistHz())));
}

void SpectrogramView::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(kBackground));
    const QRectF plot = plotRect();
    if (m_grid.isNull()) {
        painter.setPen(QColor(255, 255, 255, 150));
        painter.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(plot, m_grid);

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    painter.setFont(small);
    const QFontMetricsF metrics(small);
    const double nyquist = m_spectrogram.nyquistHz();
    const auto yFor = [&plot, nyquist](double hz) { return plot.bottom() - (hz / nyquist * plot.height()); };

    // Frequency ticks, as far apart as the height allows.
    const double pointsPerKiloHertz = plot.height() * 1000.0 / nyquist;
    int step = kTickSteps.back();
    for (const int candidate : kTickSteps) {
        if (candidate * pointsPerKiloHertz >= kMinTickGap) {
            step = candidate;
            break;
        }
    }
    const bool cutoffShown = m_cutoffHz && *m_cutoffHz > 0 && *m_cutoffHz < nyquist;
    const qreal cutoffY = cutoffShown ? yFor(*m_cutoffHz) : -1.0;
    for (int k = step; k * 1000.0 < nyquist; k += step) {
        const qreal y = yFor(k * 1000.0);
        painter.setPen(QColor(255, 255, 255, 36));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const qreal baseline = y - 3;
        if (cutoffShown && cutoffY >= baseline - metrics.ascent() - 2 && cutoffY <= baseline + metrics.descent() + 2) {
            continue; // the cut-off line would strike the label through
        }
        painter.setPen(QColor(255, 255, 255, 170));
        painter.drawText(QPointF(plot.left() + 4, baseline), u"%1 kHz"_s.arg(k));
    }

    // The cut-off: a dashed line, and its frequency in words beside it.
    if (cutoffShown) {
        const qreal y = cutoffY;
        painter.setPen(QPen(QColor(255, 255, 255, 230), 1.0, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const QString label = tr("cut-off %1").arg(kiloHertz(*m_cutoffHz));
        const qreal width = metrics.horizontalAdvance(label);
        const bool below = y - metrics.height() < plot.top(); // no room above it
        const QRectF box(plot.right() - width - 10, below ? y + 2 : y - metrics.height() - 2, width + 6,
                         metrics.height());
        painter.fillRect(box, QColor(11, 12, 15, 190));
        painter.setPen(QColor(255, 255, 255, 240));
        painter.drawText(box, Qt::AlignCenter, label);
    }
}

void SpectrogramView::mouseMoveEvent(QMouseEvent *event)
{
    const QString text = textAt(event->position().toPoint());
    if (text.isEmpty()) {
        QToolTip::hideText();
    } else {
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    }
    QWidget::mouseMoveEvent(event);
}

void SpectrogramView::leaveEvent(QEvent *event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

void SpectrogramView::refreshDescription()
{
    if (m_grid.isNull()) {
        setAccessibleDescription(m_message);
        return;
    }
    const QString range =
        tr("0 to %1, over %2")
            .arg(kiloHertz(m_spectrogram.nyquistHz()), format::clock(m_spectrogram.durationSeconds()));
    setAccessibleDescription(m_cutoffHz ? tr("%1; the spectrum stops at %2").arg(range, kiloHertz(*m_cutoffHz))
                                        : range);
}

} // namespace selecta::app
