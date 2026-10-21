// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QToolButton>
#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;

namespace selecta::app {

// A toolbar button as macOS 26 draws one: the symbol (and its text, for a count) in the label colour, a soft capsule
// under the mouse, the accent colour while its action is checked when `tintWhenChecked`. Takes no keyboard focus: its
// command is in a menu, with its shortcut. Main thread only.
class CapsuleButton : public QToolButton
{
    Q_OBJECT

public:
    CapsuleButton(QAction *action, bool showText, bool tintWhenChecked, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // In the accent colour, for a state the action doesn't carry (the inspector shown).
    void setTinted(bool tinted);
    bool isTinted() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    bool m_showText;
    bool m_tintWhenChecked;
    bool m_tinted = false;
    bool m_hovered = false;
};

// Buttons grouped in a glass capsule (macOS 26 toolbars). Main thread only.
class GlassCapsule : public QWidget
{
    Q_OBJECT

public:
    explicit GlassCapsule(QWidget *parent = nullptr);
    void addButton(QToolButton *button);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QHBoxLayout *m_layout;
};

// The window's toolbar (P-003), under the transparent title bar: the sidebar button, the view's title and subtitle,
// then capsules of buttons on the right. Pressing its empty space moves the window, a double-click zooms it, as the
// system's title bar does. With the sidebar hidden it starts after the traffic lights. Main thread only.
class Toolbar : public QWidget
{
    Q_OBJECT

public:
    explicit Toolbar(QWidget *parent = nullptr);

    QLabel *title() const { return m_title; }
    QLabel *subtitle() const { return m_subtitle; }

    // The button before the title (the sidebar's).
    QToolButton *setLeadingAction(QAction *action, const QString &objectName);
    // A capsule on the right holding these buttons, after the capsules already added; returns it, named `objectName`.
    GlassCapsule *addCapsule(const QString &objectName, const QList<QToolButton *> &buttons);
    // Room for the traffic lights at the start, when the sidebar no longer holds them.
    void setLeavesRoomForTrafficLights(bool leaves);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QHBoxLayout *m_layout;
    QLabel *m_title;
    QLabel *m_subtitle;
    int m_capsuleIndex; // where the next capsule goes
};

} // namespace selecta::app
