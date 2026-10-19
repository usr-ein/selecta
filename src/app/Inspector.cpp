// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Inspector.h"

#include <QLabel>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {

Inspector::Inspector(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"inspector"_s);
    setMinimumWidth(220);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);

    auto *empty = new QLabel(tr("Nothing selected"), this);
    empty->setObjectName(u"inspector.empty"_s);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->addWidget(empty);
}

} // namespace selecta::app
