// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include <chrono>
#include <functional>

class QVBoxLayout;

namespace selecta::app {

// Notifications at the top right of the window's content (the mock's toasts): something finished, with an action or
// two such as Undo. Each goes after a few seconds (not while the pointer rests on it), or when closed; at most four
// show at once, the newest on top. Main thread only.
class Toasts : public QWidget
{
    Q_OBJECT

public:
    struct Action
    {
        QString label;
        std::function<void()> run;
    };
    enum class Kind { Info, Good, Warning };

    static constexpr int kMaxShown = 4;
    static constexpr std::chrono::milliseconds kTimeout{6000};

    // Floats over `host` at its top right, following its size.
    explicit Toasts(QWidget *host);

    // Shows a notification. Returns its object name ("toast.N"); its buttons are "toast.N.action.I" and
    // "toast.N.close".
    QString show(const QString &title, const QString &body, Kind kind = Kind::Info, const QList<Action> &actions = {},
                 std::chrono::milliseconds timeout = kTimeout);
    int count() const;
    // The notifications shown, newest first: title and body.
    QStringList texts() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void dismiss(QWidget *toast);
    void place();

    QWidget *m_host;
    QVBoxLayout *m_layout;
    int m_next = 1;
};

} // namespace selecta::app
