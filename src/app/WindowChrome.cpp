// SPDX-License-Identifier: GPL-3.0-or-later
// The window chrome off macOS (WindowChrome.h): the platform's frame, no glass.
#include "app/WindowChrome.h"

#include <QWidget>
#include <QWindow>

namespace selecta::app::chrome {

bool contentUnderTitleBar()
{
    return false;
}

void prepare(QWidget * /*window*/, bool /*glass*/) {}

bool install(QWidget * /*window*/)
{
    return false;
}

bool hasGlass(const QWidget * /*window*/)
{
    return false;
}

void titleBarPressed(const QWidget *window)
{
    if (window && window->windowHandle()) {
        window->windowHandle()->startSystemMove();
    }
}

void titleBarDoubleClicked(const QWidget *window)
{
    if (window && window->windowHandle()) {
        QWindow *const native = window->windowHandle();
        native->setWindowStates(native->windowStates() ^ Qt::WindowMaximized);
    }
}

} // namespace selecta::app::chrome
