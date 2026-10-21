// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Toolbar.h"

#include "app/Appearance.h"
#include "app/Icons.h"
#include "app/Style.h"
#include "app/WindowChrome.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kButtonHeight = 28;
constexpr int kButtonMinWidth = 32;
constexpr int kCapsuleHeight = 34;
constexpr int kCapsulePadding = 3;
constexpr int kIconSize = 17;

QColor withAlpha(QColor color, float alpha)
{
    color.setAlphaF(alpha);
    return color;
}

QFont countFont(const QFont &base)
{
    QFont font = base;
    font.setWeight(QFont::DemiBold);
    font.setFeature(QFont::Tag("tnum"), 1); // counts that don't jiggle as they change
    return font;
}

} // namespace

CapsuleButton::CapsuleButton(QAction *action, bool showText, bool tintWhenChecked, QWidget *parent)
    : QToolButton(parent)
    , m_showText(showText)
    , m_tintWhenChecked(tintWhenChecked)
{
    setDefaultAction(action);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus); // the command is in a menu, with its shortcut
    setIconSize(QSize(kIconSize, kIconSize));
    setToolButtonStyle(showText ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    setCursor(Qt::ArrowCursor);
}

QSize CapsuleButton::sizeHint() const
{
    int width = kIconSize + (2 * style::kSpace2);
    if (m_showText && !text().isEmpty()) {
        width += style::kSpace1 + 1 + QFontMetrics(countFont(font())).horizontalAdvance(text());
    }
    return {std::max(width, kButtonMinWidth), kButtonHeight};
}

QSize CapsuleButton::minimumSizeHint() const
{
    return sizeHint();
}

void CapsuleButton::setTinted(bool tinted)
{
    if (m_tinted != tinted) {
        m_tinted = tinted;
        update();
    }
}

bool CapsuleButton::isTinted() const
{
    return m_tinted || (m_tintWhenChecked && isChecked());
}

void CapsuleButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QToolButton::enterEvent(event);
}

void CapsuleButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QToolButton::leaveEvent(event);
}

void CapsuleButton::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const QColor label = pal.color(QPalette::Active, QPalette::WindowText);
    if (isEnabled() && (isDown() || m_hovered)) {
        painter.fillPath(style::squircle(QRectF(rect()), height() / 2.0), withAlpha(label, isDown() ? 0.12F : 0.07F));
    }
    QColor ink = label;
    if (!isEnabled()) {
        ink = withAlpha(label, 0.28F);
    } else if (isTinted()) {
        ink = symbolAccentColor(pal);
    }
    const QPixmap glyph = icons::tinted(icon(), iconSize(), devicePixelRatioF(), ink);
    const bool withText = m_showText && !text().isEmpty();
    const QFont font = countFont(this->font());
    const int textWidth = withText ? QFontMetrics(font).horizontalAdvance(text()) : 0;
    const int content = kIconSize + (withText ? style::kSpace1 + 1 + textWidth : 0);
    int x = (width() - content) / 2;
    if (!glyph.isNull()) {
        painter.drawPixmap(QPointF(x, (height() - kIconSize) / 2.0), glyph);
    }
    if (withText) {
        x += kIconSize + style::kSpace1 + 1;
        painter.setFont(font);
        painter.setPen(ink);
        painter.drawText(QRect(x, 0, textWidth + 2, height()), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                         text());
    }
}

GlassCapsule::GlassCapsule(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QHBoxLayout(this))
{
    m_layout->setContentsMargins(kCapsulePadding, kCapsulePadding, kCapsulePadding, kCapsulePadding);
    m_layout->setSpacing(2);
    setFixedHeight(kCapsuleHeight);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void GlassCapsule::addButton(QToolButton *button)
{
    button->setParent(this);
    m_layout->addWidget(button);
}

void GlassCapsule::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const bool dark = isDarkPalette(pal);
    const QRectF body = QRectF(rect()).adjusted(1, 1, -1, -2);
    // A soft shadow under the glass, then the glass, its outer hairline and its lit rim (mock: --shadow-glass).
    for (int i = 1; i <= 2; ++i) {
        painter.fillPath(style::squircle(body.translated(0, i * 0.5).adjusted(-0.5 * i, 0, 0.5 * i, 0.5 * i),
                                         body.height() / 2.0),
                         QColor(0, 0, 0, dark ? 40 / i : 14 / i));
    }
    const QPainterPath shape = style::squircle(body, body.height() / 2.0);
    const QColor base = pal.color(QPalette::Base);
    painter.fillPath(shape, dark ? QColor(58, 58, 64, 230) : withAlpha(base.lighter(103), 0.96F));
    painter.setPen(QPen(dark ? QColor(0, 0, 0, 110) : QColor(0, 0, 0, 22), 0.75));
    painter.drawPath(style::squircle(body.adjusted(-0.4, -0.4, 0.4, 0.4), body.height() / 2.0 + 0.4));
    painter.setPen(QPen(dark ? QColor(255, 255, 255, 34) : QColor(255, 255, 255, 220), 0.75));
    painter.drawPath(style::squircle(body.adjusted(0.6, 0.6, -0.6, -0.6), (body.height() / 2.0) - 0.6));
}

Toolbar::Toolbar(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QHBoxLayout(this))
    , m_title(new QLabel(this))
    , m_subtitle(new QLabel(this))
{
    setObjectName(u"toolbar"_s);
    setFixedHeight(chrome::kTitleBarHeight);
    m_layout->setContentsMargins(style::kSpace2, 0, style::kSpace3, 0);
    m_layout->setSpacing(style::kSpace2);

    auto *titleBox = new QWidget(this);
    titleBox->setObjectName(u"toolbar.titleBox"_s);
    titleBox->setAttribute(Qt::WA_TransparentForMouseEvents); // its space moves the window like the rest
    auto *titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(style::kSpace1, 0, style::kSpace2, 0);
    titleLayout->setSpacing(0);
    m_title->setObjectName(u"toolbar.title"_s);
    m_title->setTextFormat(Qt::PlainText); // a peer's name, a search as typed
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    titleFont.setWeight(QFont::Bold);
    m_title->setFont(titleFont);
    m_subtitle->setObjectName(u"toolbar.subtitle"_s);
    m_subtitle->setTextFormat(Qt::PlainText); // may be a peer's or the server's words
    QFont subtitleFont = m_subtitle->font();
    subtitleFont.setPointSizeF(subtitleFont.pointSizeF() - 2.0);
    m_subtitle->setFont(subtitleFont);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    for (QLabel *label : {m_title, m_subtitle}) {
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long titles elide, never push the capsules
    }
    titleLayout->addStretch(1);
    titleLayout->addWidget(m_title);
    titleLayout->addWidget(m_subtitle);
    titleLayout->addStretch(1);
    m_layout->addWidget(titleBox, 1);
    m_capsuleIndex = m_layout->count();
}

QToolButton *Toolbar::setLeadingAction(QAction *action, const QString &objectName)
{
    auto *button = new CapsuleButton(action, false, false, this);
    button->setObjectName(objectName);
    m_layout->insertWidget(0, button);
    ++m_capsuleIndex;
    return button;
}

GlassCapsule *Toolbar::addCapsule(const QString &objectName, const QList<QToolButton *> &buttons)
{
    auto *capsule = new GlassCapsule(this);
    capsule->setObjectName(objectName);
    for (QToolButton *button : buttons) {
        capsule->addButton(button);
    }
    m_layout->insertWidget(m_capsuleIndex++, capsule);
    return capsule;
}

void Toolbar::setLeavesRoomForTrafficLights(bool leaves)
{
    const int left = leaves && chrome::contentUnderTitleBar() ? chrome::kTrafficLightsWidth : style::kSpace2;
    QMargins margins = m_layout->contentsMargins();
    margins.setLeft(left);
    m_layout->setContentsMargins(margins);
}

void Toolbar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        chrome::titleBarPressed(window());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void Toolbar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        chrome::titleBarDoubleClicked(window());
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace selecta::app
