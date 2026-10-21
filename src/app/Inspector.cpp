// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Inspector.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

Inspector::Inspector(QWidget *parent)
    : QWidget(parent)
    , m_stack(new QStackedWidget(this))
    , m_empty(new QLabel(tr("Nothing selected"), this))
{
    setObjectName(u"inspector"_s);
    setMinimumWidth(220);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    applyPanelPalette();

    m_empty->setObjectName(u"inspector.empty"_s);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    m_stack->addWidget(m_empty);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
}

QColor Inspector::panelColor(const QPalette &palette)
{
    const QColor base = palette.color(QPalette::Base);
    const QColor window = palette.color(QPalette::Window);
    constexpr float kTowardWindow = 0.3F;
    return QColor::fromRgbF(base.redF() + ((window.redF() - base.redF()) * kTowardWindow),
                            base.greenF() + ((window.greenF() - base.greenF()) * kTowardWindow),
                            base.blueF() + ((window.blueF() - base.blueF()) * kTowardWindow));
}

void Inspector::applyPanelPalette()
{
    // From the application's palette, not this widget's own: this one is set here.
    QPalette panel = QApplication::palette(this);
    const QColor colour = panelColor(panel);
    panel.setColor(QPalette::Window, colour);
    panel.setColor(QPalette::Base, colour); // the panels' scroll areas
    setPalette(panel);
}

void Inspector::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        applyPanelPalette(); // light and dark
    }
}

void Inspector::addPanel(const QString &viewId, QWidget *panel)
{
    Q_ASSERT_X(!m_panels.contains(viewId), "Inspector::addPanel", "one panel per view");
    m_panels.insert(viewId, panel);
    m_stack->addWidget(panel);
}

void Inspector::showPanelFor(const QString &viewId)
{
    QWidget *panel = m_panels.value(viewId, nullptr);
    m_stack->setCurrentWidget(panel ? panel : m_empty);
}

QWidget *Inspector::currentPanel() const
{
    return m_stack->currentWidget();
}

void Inspector::fitToWidth(QScrollArea *scroll)
{
    if (!scroll) {
        return;
    }
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const QWidget *details = scroll->widget();
    if (!details) {
        return;
    }
    const QList<QLabel *> labels = details->findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->wordWrap()) {
            QSizePolicy policy = label->sizePolicy(); // keeps height-for-width
            policy.setHorizontalPolicy(QSizePolicy::Ignored);
            label->setSizePolicy(policy);
        }
    }
}

} // namespace selecta::app
