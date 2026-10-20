// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/WaveformView.h"

#include "app/Format.h"

#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

// rekordbox's 3-band colours, as measured (references/audio-preview-and-waveform.md §2.1).
constexpr QRgb kBackground = qRgb(0x0b, 0x0c, 0x0f);
constexpr QRgb kLow = qRgb(0x00, 0x55, 0xe1);
constexpr QRgb kMid = qRgb(0xff, 0xa6, 0x00);
constexpr QRgb kOverlap = qRgb(0xb3, 0x66, 0x07);
constexpr QRgb kHigh = qRgb(0xff, 0xff, 0xff);
constexpr float kHighOpacity = 0.85F; // detail highs sit in front, slightly translucent

// Detail heights, as fractions of half the view: lows and mids each from the centre (overlaid, not added).
constexpr float kDetailLow = 0.96F;
constexpr float kDetailMid = 0.85F;
constexpr float kDetailHigh = 0.5F;
// Overview: rekordbox's preview stacks low · 0.49, then mid · 0.32, then high · 0.25 from the bottom.
constexpr float kOverviewLow = 0.49F;
constexpr float kOverviewMid = 0.32F;
constexpr float kOverviewHigh = 0.25F;
constexpr float kOverviewTotal = kOverviewLow + kOverviewMid + kOverviewHigh;
// Display curves on top of the √-companded bytes: a little lift for mids and highs, which carry less energy.
constexpr double kMidCurve = 0.85;
constexpr double kHighCurve = 0.8;
// A quiet track is normalised to its loudest column, but never boosted more than 4×: noise stays small.
constexpr int kGainFloor = 64;

constexpr int kOverviewHeight = 30;
constexpr int kGap = 6;

QRgb blend(QRgb under, QRgb over, float opacity)
{
    const auto mix = [opacity](int a, int b) {
        return static_cast<int>(
            std::lround((static_cast<float>(b) * opacity) + (static_cast<float>(a) * (1.0F - opacity))));
    };
    return qRgb(mix(qRed(under), qRed(over)), mix(qGreen(under), qGreen(over)), mix(qBlue(under), qBlue(over)));
}

// Paints rows [top, bottom) of column x.
void fillColumn(QImage &image, int x, int top, int bottom, QRgb color, float opacity = 1.0F)
{
    top = std::max(top, 0);
    bottom = std::min(bottom, image.height());
    for (int y = top; y < bottom; ++y) {
        auto *pixel = reinterpret_cast<QRgb *>(image.scanLine(y)) + x;
        *pixel = opacity >= 1.0F ? color : blend(*pixel, color, opacity);
    }
}

void hatch(QPainter &painter, const QRectF &area)
{
    if (area.width() <= 0) {
        return;
    }
    painter.save();
    painter.setClipRect(area);
    painter.setPen(QPen(QColor(255, 255, 255, 26), 1));
    const qreal height = area.height();
    const auto lines = static_cast<int>(std::ceil((area.width() + height) / 7.0));
    for (int line = 0; line < lines; ++line) {
        const qreal x = area.left() - height + (line * 7.0);
        painter.drawLine(QPointF(x, area.bottom()), QPointF(x + height, area.top()));
    }
    painter.fillRect(QRectF(area.left(), area.center().y() - 0.5, area.width(), 1), QColor(255, 255, 255, 51));
    painter.restore();
}

// A small label with a dark backing, for times drawn over the waveform.
void label(QPainter &painter, const QString &text, QPointF anchor, bool alignRight)
{
    const QFontMetricsF metrics(painter.font());
    const qreal width = metrics.horizontalAdvance(text) + 8;
    const qreal left = alignRight ? anchor.x() - width : anchor.x();
    const QRectF box(left, anchor.y(), width, metrics.height() + 2);
    painter.fillRect(box, QColor(0, 0, 0, 158));
    painter.setPen(QColor(255, 255, 255, 235));
    painter.drawText(box, Qt::AlignCenter, text);
}

} // namespace

WaveformView::WaveformView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"player.waveform"_s);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setAccessibleName(tr("Waveform"));
    setAccessibleDescription(tr("Drag the zoomed view, or click the overview, to move through the track."));
}

QSize WaveformView::sizeHint() const
{
    return {640, 132};
}

QSize WaveformView::minimumSizeHint() const
{
    return {200, 96};
}

void WaveformView::setWaveform(const audio::Waveform &waveform)
{
    m_waveform = waveform;
    updateGains();
    m_overviewCache = {};
    update();
}

void WaveformView::setDuration(double seconds)
{
    const double duration = std::max(0.0, seconds);
    if (std::abs(duration - m_duration) > 1e-6) {
        m_duration = duration;
        m_overviewCache = {};
        update();
    }
}

void WaveformView::setPosition(double seconds)
{
    if (seconds != m_position) {
        m_position = seconds;
        update();
    }
}

void WaveformView::setBuffered(double seconds)
{
    if (seconds != m_buffered) {
        m_buffered = seconds;
        update();
    }
}

void WaveformView::setMessage(const QString &message)
{
    if (message != m_message) {
        m_message = message;
        update();
    }
}

void WaveformView::clear()
{
    m_waveform = {};
    m_gain = {1.0F, 1.0F, 1.0F};
    m_duration = 0.0;
    m_position = 0.0;
    m_buffered = -1.0;
    m_overviewCache = {};
    m_drag = Drag::None;
    m_dragPosition.reset();
    update();
}

QRect WaveformView::detailRect() const
{
    return {0, 0, width(), std::max(0, height() - kOverviewHeight - kGap)};
}

QRect WaveformView::overviewRect() const
{
    return {0, height() - kOverviewHeight, width(), kOverviewHeight};
}

double WaveformView::duration() const
{
    if (m_duration > 0.0) {
        return m_duration;
    }
    return m_waveform.isComplete() ? m_waveform.durationSeconds() : 0.0;
}

double WaveformView::shownPosition() const
{
    return m_dragPosition.value_or(m_position);
}

double WaveformView::timeAtOverview(int x) const
{
    const QRect rect = overviewRect();
    const double fraction = rect.width() > 0 ? static_cast<double>(x - rect.left()) / rect.width() : 0.0;
    return std::clamp(fraction, 0.0, 1.0) * duration();
}

void WaveformView::updateGains()
{
    std::array<int, audio::kBandCount> loudest{};
    const auto note = [&loudest](const auto &peaks) {
        for (int band = 0; band < audio::kBandCount; ++band) {
            loudest.at(band) = std::max<int>(loudest.at(band), peaks.at(band));
        }
    };
    if (m_waveform.hasDetail()) {
        for (const audio::WaveformColumn &column : m_waveform.detail()) {
            note(column.peak);
        }
    } else {
        for (const audio::OverviewColumn &column : m_waveform.overview()) {
            note(column.peak);
        }
    }
    for (int band = 0; band < audio::kBandCount; ++band) {
        m_gain.at(band) = 255.0F / static_cast<float>(std::max(loudest.at(band), kGainFloor));
    }
}

WaveformView::Levels WaveformView::normalised(std::array<int, audio::kBandCount> peaks) const
{
    const auto level = [this, &peaks](audio::Band band) {
        const int index = audio::bandIndex(band);
        return std::min(1.0F, static_cast<float>(peaks.at(index)) * m_gain.at(index) / 255.0F);
    };
    return {level(audio::Band::Low), static_cast<float>(std::pow(level(audio::Band::Mid), kMidCurve)),
            static_cast<float>(std::pow(level(audio::Band::High), kHighCurve))};
}

WaveformView::Levels WaveformView::levelsBetween(double from, double to) const
{
    if (m_waveform.isNull() || to <= 0.0) {
        return {};
    }
    std::array<int, audio::kBandCount> peaks{};
    const auto take = [&peaks](const auto &columnPeaks) {
        for (int band = 0; band < audio::kBandCount; ++band) {
            peaks.at(band) = std::max<int>(peaks.at(band), columnPeaks.at(band));
        }
    };
    if (m_waveform.hasDetail()) {
        const QList<audio::WaveformColumn> &detail = m_waveform.detail();
        const auto first = static_cast<qint64>(std::floor(from * audio::kColumnsPerSecond));
        auto last = static_cast<qint64>(std::floor(to * audio::kColumnsPerSecond));
        if (first < 0 || first >= detail.size()) {
            return {}; // before the start, or not analysed yet
        }
        last = std::clamp(last, first + 1, static_cast<qint64>(detail.size()));
        for (qint64 column = first; column < last; ++column) {
            take(detail.at(column).peak);
        }
    } else {
        // Overview only (the long-term cache): its columns span the whole track.
        const QList<audio::OverviewColumn> &overview = m_waveform.overview();
        const double length = m_waveform.durationSeconds();
        if (overview.isEmpty() || length <= 0.0 || from < 0.0 || from >= length) {
            return {};
        }
        const auto count = static_cast<double>(overview.size());
        const auto first = static_cast<qint64>(std::floor(from / length * count));
        const qint64 last = std::clamp(static_cast<qint64>(std::floor(to / length * count)), first + 1,
                                       static_cast<qint64>(overview.size()));
        for (qint64 column = first; column < last; ++column) {
            take(overview.at(column).peak);
        }
    }
    return normalised(peaks);
}

QImage WaveformView::renderDetail(QSize pixels, double from, double to) const
{
    QImage image(pixels, QImage::Format_RGB32);
    image.fill(kBackground);
    if (m_waveform.isNull() || pixels.isEmpty()) {
        return image;
    }
    const double step = (to - from) / pixels.width();
    const float middle = static_cast<float>(pixels.height()) / 2.0F;
    const float half = middle - 1.0F;
    const auto top = [middle](float extent) { return static_cast<int>(std::lround(middle - extent)); };
    const auto bottom = [middle](float extent) { return static_cast<int>(std::lround(middle + extent)); };
    for (int x = 0; x < pixels.width(); ++x) {
        const double start = from + (x * step);
        const Levels levels = levelsBetween(start, start + step);
        if (levels.low < 0.0F) {
            continue;
        }
        const float low = levels.low * kDetailLow * half;
        const float mid = levels.mid * kDetailMid * half;
        const float overlap = std::min(low, mid);
        const float larger = std::max(low, mid);
        fillColumn(image, x, top(overlap), bottom(overlap), kOverlap);
        const QRgb outer = low > mid ? kLow : kMid;
        fillColumn(image, x, top(larger), top(overlap), outer);
        fillColumn(image, x, bottom(overlap), bottom(larger), outer);
        const float high = levels.high * kDetailHigh * half;
        fillColumn(image, x, top(high), bottom(high), kHigh, kHighOpacity);
    }
    return image;
}

QImage WaveformView::renderOverview(QSize pixels) const
{
    QImage image(pixels, QImage::Format_RGB32);
    image.fill(kBackground);
    const double length = duration();
    if (m_waveform.isNull() || pixels.isEmpty() || length <= 0.0) {
        return image;
    }
    const double step = length / pixels.width();
    const float scale = static_cast<float>(pixels.height() - 2) / kOverviewTotal;
    const int floor = pixels.height();
    for (int x = 0; x < pixels.width(); ++x) {
        const double start = x * step;
        const Levels levels = levelsBetween(start, start + step);
        if (levels.low < 0.0F) {
            continue;
        }
        const float low = levels.low * kOverviewLow * scale;
        const float mid = low + (levels.mid * kOverviewMid * scale);
        const float high = mid + (levels.high * kOverviewHigh * scale);
        fillColumn(image, x, floor - static_cast<int>(std::lround(high)), floor, kHigh);
        fillColumn(image, x, floor - static_cast<int>(std::lround(mid)), floor, kMid);
        fillColumn(image, x, floor - static_cast<int>(std::lround(low)), floor, kLow);
    }
    return image;
}

void WaveformView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(kBackground));
    const qreal ratio = devicePixelRatioF();

    const QRect detail = detailRect();
    const double position = shownPosition();
    const double from = position - (kZoomSeconds / 2.0);
    const double to = position + (kZoomSeconds / 2.0);
    if (!detail.isEmpty()) {
        QImage bands = renderDetail((QSizeF(detail.size()) * ratio).toSize(), from, to);
        bands.setDevicePixelRatio(ratio);
        painter.drawImage(detail.topLeft(), bands);
        paintDetailOverlay(painter, detail, from, to);
    }

    const QRect overview = overviewRect();
    if (!overview.isEmpty()) {
        const QSize pixels = (QSizeF(overview.size()) * ratio).toSize();
        if (m_overviewCache.size() != pixels) {
            m_overviewCache = renderOverview(pixels);
            m_overviewCache.setDevicePixelRatio(ratio);
        }
        painter.drawImage(overview.topLeft(), m_overviewCache);
        paintOverviewOverlay(painter, overview);
    }
}

void WaveformView::paintDetailOverlay(QPainter &painter, QRect rect, double from, double to) const
{
    const double length = duration();
    if (length <= 0.0 && m_waveform.isNull()) {
        if (!m_message.isEmpty()) {
            painter.setPen(QColor(255, 255, 255, 102));
            painter.drawText(rect.adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        }
        return;
    }
    const auto xAt = [&rect, from, to](double seconds) {
        return rect.left() + ((seconds - from) / (to - from) * rect.width());
    };
    if (m_buffered >= 0.0 && length > 0.0 && m_buffered < length) {
        const qreal left = std::max<qreal>(rect.left(), xAt(m_buffered));
        const qreal right = std::min<qreal>(rect.right() + 1, xAt(length));
        hatch(painter, QRectF(left, rect.top(), right - left, rect.height()));
    }

    // The playhead: fixed in the middle, the waveform moves under it.
    const qreal centre = rect.left() + (rect.width() / 2.0);
    painter.fillRect(QRectF(centre - 1, rect.top(), 2, rect.height()), Qt::white);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    QPainterPath marks;
    const auto top = static_cast<qreal>(rect.top());
    marks.addPolygon(QPolygonF({{centre - 5, top}, {centre + 5, top}, {centre, top + 6.0}}));
    marks.addPolygon(QPolygonF(
        {{centre - 5, rect.bottom() + 1.0}, {centre + 5, rect.bottom() + 1.0}, {centre, rect.bottom() - 5.0}}));
    painter.drawPath(marks);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(10);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    const double position = shownPosition();
    label(painter, format::clock(position, 1), QPointF(rect.left() + 4, rect.top() + 3), false);
    if (length > 0.0) {
        label(painter, u"−"_s + format::clock(std::max(0.0, length - position), 1),
              QPointF(rect.right() - 3, rect.top() + 3), true);
    }
}

void WaveformView::paintOverviewOverlay(QPainter &painter, QRect rect) const
{
    const double length = duration();
    if (length <= 0.0) {
        return;
    }
    const auto xAt = [&rect, length](double seconds) {
        return rect.left() + (std::clamp(seconds / length, 0.0, 1.0) * rect.width());
    };
    const qreal playhead = xAt(shownPosition());
    painter.fillRect(QRectF(rect.left(), rect.top(), playhead - rect.left(), rect.height()), QColor(0, 0, 0, 107));

    if (m_buffered >= 0.0 && m_buffered < length) {
        const qreal left = xAt(m_buffered);
        hatch(painter, QRectF(left, rect.top(), rect.right() + 1 - left, rect.height()));
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, 191));
        painter.drawText(rect.adjusted(0, 0, -5, 0), Qt::AlignRight | Qt::AlignVCenter,
                         u"%1 %"_s.arg(static_cast<int>(std::floor(m_buffered / length * 100.0))));
    }
    for (int minute = 60; minute < length; minute += 60) {
        painter.fillRect(QRectF(std::round(xAt(minute)), rect.bottom() - 3, 1, 4), QColor(255, 255, 255, 71));
    }
    painter.fillRect(QRectF(std::round(playhead) - 1, rect.top(), 2, rect.height()), Qt::white);
    if (m_hoverX && m_drag == Drag::None) {
        painter.fillRect(QRectF(*m_hoverX, rect.top(), 1, rect.height()), QColor(255, 255, 255, 128));
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPixelSize(10);
        painter.setFont(font);
        const QString time = format::clock(timeAtOverview(*m_hoverX), 0);
        const bool right = *m_hoverX > rect.center().x();
        label(painter, time, QPointF(right ? *m_hoverX - 3 : *m_hoverX + 3, rect.top() + 2), right);
    }
}

void WaveformView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_overviewCache = {};
}

void WaveformView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::DevicePixelRatioChange) {
        m_overviewCache = {};
    }
    QWidget::changeEvent(event);
}

void WaveformView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || duration() <= 0.0) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint point = event->position().toPoint();
    if (overviewRect().contains(point)) {
        m_drag = Drag::Overview;
        m_dragPosition = timeAtOverview(point.x());
    } else if (detailRect().contains(point)) {
        m_drag = Drag::Detail;
        m_dragStartX = point.x();
        m_dragStartPosition = m_position;
        m_dragPosition = m_position;
        setCursor(Qt::ClosedHandCursor);
    }
    update();
}

void WaveformView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint point = event->position().toPoint();
    switch (m_drag) {
    case Drag::Overview:
        m_dragPosition = timeAtOverview(point.x());
        break;
    case Drag::Detail: {
        const double dragged = static_cast<double>(point.x() - m_dragStartX) / std::max(1, detailRect().width());
        m_dragPosition = std::clamp(m_dragStartPosition - (dragged * kZoomSeconds), 0.0, duration());
        break;
    }
    case Drag::None:
        if (duration() > 0.0 && overviewRect().contains(point)) {
            m_hoverX = point.x();
            setCursor(Qt::PointingHandCursor);
        } else {
            m_hoverX.reset();
            setCursor(duration() > 0.0 && detailRect().contains(point) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        }
        break;
    }
    update();
}

void WaveformView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_drag == Drag::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const std::optional<double> target = m_dragPosition;
    m_drag = Drag::None;
    m_dragPosition.reset();
    unsetCursor();
    if (target) {
        m_position = *target; // until the player reports the new position
        Q_EMIT seekRequested(*target);
    }
    update();
}

void WaveformView::leaveEvent(QEvent *event)
{
    m_hoverX.reset();
    update();
    QWidget::leaveEvent(event);
}

} // namespace selecta::app
