// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QLabel;
class QStackedWidget;

namespace selecta::app {

// The right-hand details pane (PRD 8.2). A view registers a panel under its id; the window shows the panel of the
// current view, or "Nothing selected" for views without one. Panels follow their view's selection themselves.
class Inspector : public QWidget
{
    Q_OBJECT

public:
    explicit Inspector(QWidget *parent = nullptr);

    // Adds a view's panel (the Inspector takes it as a child). One panel per view id.
    void addPanel(const QString &viewId, QWidget *panel);
    // Shows the panel registered for this view, or the empty state.
    void showPanelFor(const QString &viewId);
    QWidget *currentPanel() const;

private:
    QStackedWidget *m_stack;
    QLabel *m_empty;
    QHash<QString, QWidget *> m_panels;
};

} // namespace selecta::app
