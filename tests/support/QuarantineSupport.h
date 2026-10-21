// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/Quarantine.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#ifdef Q_OS_MACOS
#include <sys/xattr.h>
#endif
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

// Test-only: quarantine attributes as other apps leave them, so tests can check that Selecta keeps them.
namespace selecta::testing {

// Safari's attribute on a file the user allowed to open (0x0040): Selecta must never replace it.
inline QByteArray safariApproved()
{
    return QByteArrayLiteral("00c1;5f5e1000;Safari;0A1B2C3D-0000-4000-8000-000000000001");
}

// Puts `value` on `path` as its quarantine attribute. False where the platform has none.
inline bool setQuarantine(const QString &path, const QByteArray &value)
{
#ifdef Q_OS_MACOS
    return setxattr(QFile::encodeName(path).constData(), library::quarantine::kAttribute, value.constData(),
                    static_cast<size_t>(value.size()), 0, XATTR_NOFOLLOW) == 0;
#else
    Q_UNUSED(path);
    Q_UNUSED(value);
    return false;
#endif
}

// True when the tests run as root, whom read-only files don't stop (Unix). False elsewhere.
inline bool runningAsRoot()
{
#ifdef Q_OS_UNIX
    return ::geteuid() == 0;
#else
    return false;
#endif
}

} // namespace selecta::testing
