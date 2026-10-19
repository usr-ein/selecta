// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

namespace selecta::app {

// The right-hand details pane (PRD 8.2). Views fill it with details of their selection; with nothing selected it says
// so.
class Inspector : public QWidget
{
    Q_OBJECT

public:
    explicit Inspector(QWidget *parent = nullptr);
};

} // namespace selecta::app
