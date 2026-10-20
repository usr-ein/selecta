// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/WishlistService.h"

#include <QWidget>

class QLineEdit;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// The Wishlist (PRD SRCH-14): searches that run again in the background, with when each last ran and how many new
// files it found. Show opens the search (and counts its finds as seen); new ones are added from the field at the top,
// from a search ("Add to Wishlist") or from Get tracks. Main thread only.
class WishlistView : public QWidget
{
    Q_OBJECT

public:
    explicit WishlistView(core::EngineHost &engine, QWidget *parent = nullptr);

    QString summary() const;
    // How many new files are waiting, for the sidebar.
    int foundCount() const;

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void summaryChanged();
    // Show: open this search.
    void searchRequested(const QString &text);

private:
    void rebuild(const QList<core::WishlistItem> &items);
    void showSelected();
    void addTyped();
    QList<qint64> selectedIds() const;

    core::EngineHost &m_engine;
    QList<core::WishlistItem> m_items;
    QLineEdit *m_field;
    QPushButton *m_add;
    QStackedWidget *m_pages;
    QStandardItemModel *m_model;
    QTreeView *m_table;
};

} // namespace selecta::app
