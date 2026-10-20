// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

class QAbstractItemView;
class QLineEdit;

namespace selecta::app::a11y {

// VoiceOver and the keyboard (PRD UI-5): small helpers for what Qt leaves out. Main thread.

// Turns on the field's clear button and names it "Clear": Qt leaves it nameless, and VoiceOver would read "button".
void addClearButton(QLineEdit *field);

// ⌃Return (and the PC keyboard's Menu key) opens the view's context menu for its current row, as a right-click on it
// would: macOS has no key for it outside VoiceOver, and some commands live only there (Copy Path, the peer's menu).
void addMenuKey(QAbstractItemView *view);

// Return (and Enter) with focus in the view runs `action`, as a double-click on a row does. On macOS an item view
// keeps Return for renaming, so without this a list that acts on double-click can't be used from the keyboard.
void onReturn(QAbstractItemView *view, const std::function<void()> &action);

} // namespace selecta::app::a11y
