// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QIcon>
#include <QWidget>

class QLabel;

namespace selecta::app {

// A centred empty state: symbol, title, explanation and an optional note (e.g. which milestone builds the feature).
// Shows what a view will contain without pretending to work.
class EmptyStateView : public QWidget
{
    Q_OBJECT

public:
    EmptyStateView(QIcon symbol, const QString &title, const QString &text, const QString &note = {},
                   QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void refreshSymbol();

    QIcon m_symbol;
    QLabel *m_symbolLabel;
};

} // namespace selecta::app
