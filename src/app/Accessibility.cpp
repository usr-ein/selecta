// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Accessibility.h"

#include <QAbstractItemView>
#include <QAction>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QLineEdit>
#include <QShortcut>

using namespace Qt::StringLiterals;

namespace selecta::app::a11y {

void addClearButton(QLineEdit *field)
{
    field->setClearButtonEnabled(true);
    // Qt's own name for the action behind the button; the button takes its text, and so its name, from it.
    if (auto *clear = field->findChild<QAction *>(u"_q_qlineeditclearaction"_s)) {
        clear->setText(QCoreApplication::translate("selecta::app::a11y", "Clear"));
    }
}

void addMenuKey(QAbstractItemView *view)
{
    for (const QKeySequence &keys : {QKeySequence(Qt::META | Qt::Key_Return), QKeySequence(Qt::Key_Menu)}) {
        auto *shortcut = new QShortcut(keys, view);
        shortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(shortcut, &QShortcut::activated, view, [view] {
            const QModelIndex current = view->currentIndex();
            const QRect row = current.isValid() ? view->visualRect(current) : QRect();
            const QPoint position = row.isValid() ? row.center() : QPoint(0, 0);
            if (view->contextMenuPolicy() == Qt::CustomContextMenu) {
                Q_EMIT view->customContextMenuRequested(position);
                return;
            }
            QContextMenuEvent event(QContextMenuEvent::Keyboard, position, view->viewport()->mapToGlobal(position));
            QCoreApplication::sendEvent(view->viewport(), &event);
        });
    }
}

void onReturn(QAbstractItemView *view, const std::function<void()> &action)
{
    for (const QKeySequence &keys : {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}) {
        auto *shortcut = new QShortcut(keys, view);
        shortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(shortcut, &QShortcut::activated, view, action);
    }
}

} // namespace selecta::app::a11y
