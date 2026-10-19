// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QListView>

namespace selecta::app {

// A macOS-style source list over a NavModel. Keyboard navigation skips section headers, which can't be selected.
class SourceListView : public QListView
{
    Q_OBJECT

public:
    explicit SourceListView(QWidget *parent = nullptr);

protected:
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
};

} // namespace selecta::app
