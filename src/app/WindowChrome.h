// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QWidget;

// The macOS 26 window (P-003): the content runs under a transparent title bar as tall as the toolbar, with the traffic
// lights in the sidebar's top corner; and behind the window, the system's glass (NSVisualEffectView, material Sidebar,
// blending behind the window), which shows wherever Selecta paints nothing: the sidebar's column. Off macOS these do
// nothing and the window keeps its platform frame. Main thread only.
namespace selecta::app::chrome {

// The height of the title bar the content runs under, and so of the toolbar: the traffic lights sit in its middle.
inline constexpr int kTitleBarHeight = 52;
// How far the traffic lights reach from the window's left edge, with the space after them: the toolbar starts after
// them when the sidebar is hidden.
inline constexpr int kTrafficLightsWidth = 86;

// Whether this platform draws the title bar over the content (macOS): the toolbar and the sidebar leave room for the
// traffic lights only then.
bool contentUnderTitleBar();

// Before the window is first shown: lets its content run under the title bar, and, with `glass`, makes it translucent
// so the glass shows through. Glass is for windows on screen: headless runs and tests leave it out, and paint the
// sidebar's column with the window colour instead.
void prepare(QWidget *window, bool glass);

// Once the window has its native window (after show): the title bar as tall as the toolbar (an empty unified toolbar),
// its title hidden, and, if prepared with glass, the glass behind the content. Safe to call again. Returns whether
// the glass is in place.
bool install(QWidget *window);

// Whether install() put the glass behind this window.
bool hasGlass(const QWidget *window);

// A press on empty title bar space (the toolbar's, the sidebar's top): the window follows the mouse, as when dragging
// a title bar. The buttons there take their clicks themselves.
void titleBarPressed(const QWidget *window);
// A double-click there: what the system's setting says a title bar does (macOS: zoom, minimise or nothing, as set in
// Desktop & Dock), else maximise or restore.
void titleBarDoubleClicked(const QWidget *window);

} // namespace selecta::app::chrome
