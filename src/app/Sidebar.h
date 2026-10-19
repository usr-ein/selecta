// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;

namespace selecta::app {

class NavModel;
class SourceListView;
class StatusDot;

// The left column: the source list and, at the bottom, the connection status.
class Sidebar : public QWidget
{
    Q_OBJECT

public:
    explicit Sidebar(NavModel *model, QWidget *parent = nullptr);

    // Selects the item without emitting navigationRequested. Unknown ids clear the selection.
    void select(const QString &id);
    QString currentId() const;

    void setConnectionStatus(const QString &text, bool online);

Q_SIGNALS:
    // The user picked an item (click or keyboard).
    void navigationRequested(const QString &id);

private:
    NavModel *m_model;
    SourceListView *m_list;
    StatusDot *m_statusDot;
    QLabel *m_statusText;
    bool m_selecting = false;
};

// A small round status light. Offline uses the palette's secondary text colour, read at paint time so it follows
// appearance changes.
class StatusDot : public QWidget
{
    Q_OBJECT

public:
    explicit StatusDot(QWidget *parent = nullptr);

    bool isOnline() const { return m_online; }
    void setOnline(bool online);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool m_online = false;
};

} // namespace selecta::app
