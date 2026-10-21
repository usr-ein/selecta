// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/Quarantine.h"

#include <QByteArrayList>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>

#ifdef Q_OS_MACOS
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/xattr.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::library::quarantine {
namespace {

#ifdef Q_OS_MACOS

// Quarantine values are about 60 bytes ("0083;66f1c2a0;Safari;F4A1…"): anything much longer isn't one.
constexpr ssize_t kMaxValueBytes = 1024;
// QTN_FLAG_DOWNLOAD (0x0001), plus 0x0080, which LaunchServices sets on what it quarantines for downloaders.
constexpr QLatin1StringView kFlags{"0081"};

QString describeErrno(int code)
{
    std::array<char, 256> buffer{};
    if (strerror_r(code, buffer.data(), buffer.size()) != 0) {
        return u"error %1"_s.arg(code);
    }
    return QString::fromLocal8Bit(buffer.data());
}

// "0081;6ad753b3;Selecta;": no event id, as Selecta records no event in the user's quarantine database.
QByteArray selectaValue()
{
    const QByteArray seconds = QByteArray::number(QDateTime::currentSecsSinceEpoch(), 16).rightJustified(8, '0');
    return QByteArray(kFlags.data(), kFlags.size()) + ';' + seconds + ';' + QByteArray(kAgent.data(), kAgent.size()) +
           ';';
}

// The attribute's size in bytes; -1 without one.
ssize_t attributeSize(const QByteArray &native)
{
    return getxattr(native.constData(), kAttribute, nullptr, 0, 0, XATTR_NOFOLLOW);
}

bool write(const QString &path, const QByteArray &value, QString *error)
{
    const QByteArray native = QFile::encodeName(path);
    if (setxattr(native.constData(), kAttribute, value.constData(), static_cast<size_t>(value.size()), 0,
                 XATTR_NOFOLLOW) == 0) {
        return true;
    }
    const int code = errno;
    if (error) {
        *error = u"can't mark %1 as downloaded: %2"_s.arg(QDir::toNativeSeparators(path), describeErrno(code));
    }
    return false;
}

#endif

} // namespace

bool isSupported()
{
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}

QByteArray value(const QString &path)
{
#ifdef Q_OS_MACOS
    const QByteArray native = QFile::encodeName(path);
    const ssize_t size = attributeSize(native);
    if (size <= 0 || size > kMaxValueBytes) {
        return {};
    }
    QByteArray buffer(size, Qt::Uninitialized);
    const ssize_t read =
        getxattr(native.constData(), kAttribute, buffer.data(), static_cast<size_t>(buffer.size()), 0, XATTR_NOFOLLOW);
    if (read <= 0) {
        return {}; // gone, or grown, since it was measured
    }
    buffer.truncate(read);
    return buffer;
#else
    Q_UNUSED(path);
    return {};
#endif
}

QString agentOf(const QString &path)
{
    const QList<QByteArray> fields = value(path).split(';');
    return fields.size() > 2 ? QString::fromUtf8(fields.at(2)) : QString();
}

#ifdef Q_OS_MACOS
bool mark(const QString &path, QString *error)
{
    if (attributeSize(QFile::encodeName(path)) > 0) {
        return true; // kept as it is: it may record that the user allowed the file to open
    }
    return write(path, selectaValue(), error);
}
#else
bool mark(const QString & /*path*/, QString * /*error*/)
{
    return true; // no attribute on this platform: nothing to mark, nothing to report
}
#endif

bool markTree(const QString &folder, QString *error)
{
    bool ok = mark(folder, error);
    // Links are left alone: everything Selecta puts in a folder is a plain file or folder.
    QDirIterator entries(folder,
                         QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        QString why;
        if (!mark(entries.next(), &why)) {
            if (ok && error) {
                *error = why;
            }
            ok = false;
        }
    }
    return ok;
}

#ifdef Q_OS_MACOS
bool copy(const QString &from, const QString &to, QString *error)
{
    const QByteArray attribute = value(from);
    return attribute.isEmpty() || write(to, attribute, error);
}
#else
bool copy(const QString & /*from*/, const QString & /*to*/, QString * /*error*/)
{
    return true; // no attribute on this platform: nothing to copy
}
#endif

} // namespace selecta::library::quarantine
