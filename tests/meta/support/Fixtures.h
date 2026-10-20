// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

namespace selecta::meta::testing {

// The path of a file under tests/meta/fixtures/.
inline QString fixturePath(const QString &relative)
{
    return QStringLiteral(SELECTA_META_FIXTURES) + u'/' + relative;
}

// The bytes of a fixture file; empty (and a test failure where it's used) when missing.
inline QByteArray fixture(const QString &relative)
{
    QFile file(fixturePath(relative));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace selecta::meta::testing
