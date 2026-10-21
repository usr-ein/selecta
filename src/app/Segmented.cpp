// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Segmented.h"

#include "app/Appearance.h"
#include "app/Style.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>

namespace selecta::app {
namespace {

constexpr int kTrackPadding = 2;
constexpr int kSegmentHeight = 24;

// One segment: its text, raised on a capsule when chosen.
class Segment : public QAbstractButton
{
public:
    Segment(const QString &text, QWidget *parent)
        : QAbstractButton(parent)
    {
        setText(text);
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(text);
    }

    QSize sizeHint() const override
    {
        return {fontMetrics().horizontalAdvance(text()) + (2 * style::kSpace3), kSegmentHeight};
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = palette();
        const bool dark = isDarkPalette(pal);
        const QRectF body(rect());
        if (isChecked()) {
            painter.fillPath(style::squircle(body.translated(0, 0.5), body.height() / 2.0),
                             QColor(0, 0, 0, dark ? 70 : 28));
            painter.fillPath(style::squircle(body, body.height() / 2.0),
                             dark ? QColor(255, 255, 255, 60) : pal.color(QPalette::Base));
        }
        if (hasFocus()) {
            QColor ring = symbolAccentColor(pal);
            ring.setAlphaF(0.5F);
            painter.setPen(QPen(ring, 2.0));
            painter.drawPath(style::squircle(body.adjusted(1, 1, -1, -1), (body.height() / 2.0) - 1));
        }
        QColor ink = pal.color(QPalette::Active, QPalette::WindowText);
        if (!isEnabled()) {
            ink.setAlphaF(0.3F);
        }
        painter.setPen(ink);
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextSingleLine, text());
    }
};

} // namespace

SegmentedControl::SegmentedControl(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QHBoxLayout(this))
    , m_group(new QButtonGroup(this))
{
    m_layout->setContentsMargins(kTrackPadding, kTrackPadding, kTrackPadding, kTrackPadding);
    m_layout->setSpacing(kTrackPadding);
    m_group->setExclusive(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_group, &QButtonGroup::idClicked, this, &SegmentedControl::currentChanged);
}

int SegmentedControl::addSegment(const QString &text, const QString &objectName)
{
    auto *segment = new Segment(text, this);
    segment->setObjectName(objectName);
    const int index = static_cast<int>(m_group->buttons().size());
    m_group->addButton(segment, index);
    m_layout->addWidget(segment);
    if (index == 0) {
        segment->setChecked(true);
    }
    return index;
}

int SegmentedControl::count() const
{
    return static_cast<int>(m_group->buttons().size());
}

int SegmentedControl::currentIndex() const
{
    return m_group->checkedId();
}

void SegmentedControl::setCurrentIndex(int index)
{
    if (QAbstractButton *segment = m_group->button(index)) {
        segment->setChecked(true);
    }
}

void SegmentedControl::paintEvent(QPaintEvent * /*event*/)
{
    // The track: a soft capsule (mock: --fill-2).
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor track = palette().color(QPalette::WindowText);
    track.setAlphaF(isDarkPalette(palette()) ? 0.1F : 0.065F);
    painter.fillPath(style::squircle(QRectF(rect()), height() / 2.0), track);
}

} // namespace selecta::app
