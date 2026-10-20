// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/Quarantine.h"

#include "support/QuarantineSupport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdlib>
#include <gtest/gtest.h>
#include <unistd.h>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace Qt::StringLiterals;
namespace quarantine = selecta::library::quarantine;

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

bool makeReadOnly(const QString &path)
{
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
}

// The agent macOS itself reads from the file's quarantine properties; empty when it doesn't see the file as
// quarantined.
QString agentMacOsSees(const QString &path)
{
    QString agent;
#ifdef Q_OS_MACOS
    const QByteArray native = QFile::encodeName(path);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(native.constData()), native.size(), false);
    if (url == nullptr) {
        return agent;
    }
    CFTypeRef properties = nullptr;
    if (CFURLCopyResourcePropertyForKey(url, kCFURLQuarantinePropertiesKey, static_cast<void *>(&properties),
                                        nullptr) &&
        properties != nullptr) {
        if (CFGetTypeID(properties) == CFDictionaryGetTypeID()) {
            CFStringRef key = u"LSQuarantineAgentName"_s.toCFString();
            const void *name = CFDictionaryGetValue(static_cast<CFDictionaryRef>(properties), key);
            if (name != nullptr && CFGetTypeID(name) == CFStringGetTypeID()) {
                agent = QString::fromCFString(static_cast<CFStringRef>(name));
            }
            CFRelease(key);
        }
        CFRelease(properties);
    }
    CFRelease(url);
#else
    Q_UNUSED(path);
#endif
    return agent;
}

class QuarantineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!quarantine::isSupported()) {
            GTEST_SKIP() << "this platform has no quarantine attribute";
        }
        ASSERT_TRUE(dir.isValid());
    }

    QTemporaryDir dir;
};

} // namespace

TEST(Quarantine, DoesNothingWhereThePlatformHasNone)
{
    if (quarantine::isSupported()) {
        GTEST_SKIP() << "macOS has the attribute";
    }
    const QTemporaryDir dir;
    const QString file = dir.filePath(u"track.mp3"_s);
    ASSERT_TRUE(writeFile(file, "audio"));
    EXPECT_TRUE(quarantine::mark(file));
    EXPECT_TRUE(quarantine::markTree(dir.path()));
    EXPECT_TRUE(quarantine::copy(file, file));
    EXPECT_TRUE(quarantine::value(file).isEmpty());
}

TEST_F(QuarantineTest, MarksAFileAsDownloadedBySelecta)
{
    const QString file = dir.filePath(u"track.mp3"_s);
    ASSERT_TRUE(writeFile(file, "audio"));
    EXPECT_TRUE(quarantine::value(file).isEmpty());
    EXPECT_TRUE(quarantine::agentOf(file).isEmpty());
    EXPECT_TRUE(agentMacOsSees(file).isEmpty());

    QString error;
    ASSERT_TRUE(quarantine::mark(file, &error)) << error.toStdString();
    const QList<QByteArray> fields = quarantine::value(file).split(';');
    ASSERT_EQ(fields.size(), 4) << quarantine::value(file).toStdString();
    EXPECT_EQ(fields.at(0), "0081"); // downloaded
    bool ok = false;
    const qint64 seconds = fields.at(1).toLongLong(&ok, 16);
    EXPECT_TRUE(ok);
    EXPECT_LE(std::abs(seconds - QDateTime::currentSecsSinceEpoch()), 60);
    EXPECT_EQ(fields.at(2), "Selecta");
    EXPECT_TRUE(fields.at(3).isEmpty()); // no event in the user's quarantine database
    EXPECT_EQ(quarantine::agentOf(file), u"Selecta"_s);
    EXPECT_EQ(agentMacOsSees(file), u"Selecta"_s); // macOS reads it as quarantined by Selecta
    EXPECT_EQ(QFile(file).size(), 5);              // the content is untouched
}

TEST_F(QuarantineTest, KeepsTheAttributeAFileAlreadyHas)
{
    const QString file = dir.filePath(u"from-safari.mp3"_s);
    ASSERT_TRUE(writeFile(file, "audio"));
    ASSERT_TRUE(selecta::testing::setQuarantine(file, selecta::testing::safariApproved()));
    EXPECT_TRUE(quarantine::mark(file));
    EXPECT_EQ(quarantine::value(file), selecta::testing::safariApproved()); // the user's approval stays
    EXPECT_EQ(quarantine::agentOf(file), u"Safari"_s);
}

TEST_F(QuarantineTest, MarksAFolderAndEverythingInIt)
{
    const QString album = dir.filePath(u"Okuma Drift - Night Signals (2024)"_s);
    const QStringList files{album + u"/01 - Neon Rain.mp3"_s, album + u"/cover.jpg"_s, album + u"/.hidden"_s,
                            album + u"/CD2/01 - Sodium.mp3"_s};
    for (const QString &file : files) {
        ASSERT_TRUE(writeFile(file, "x"));
    }
    QString error;
    ASSERT_TRUE(quarantine::markTree(album, &error)) << error.toStdString();
    const QStringList marked = QStringList{album, album + u"/CD2"_s} + files;
    for (const QString &path : marked) {
        EXPECT_EQ(quarantine::agentOf(path), u"Selecta"_s) << path.toStdString();
    }
}

TEST_F(QuarantineTest, ACopyTakesTheOriginalsAttribute)
{
    const QString original = dir.filePath(u"original.flac"_s);
    const QString copy = dir.filePath(u"copy.flac"_s);
    ASSERT_TRUE(writeFile(original, "audio") && writeFile(copy, "audio"));
    ASSERT_TRUE(selecta::testing::setQuarantine(original, selecta::testing::safariApproved()));
    QString error;
    ASSERT_TRUE(quarantine::copy(original, copy, &error)) << error.toStdString();
    EXPECT_EQ(quarantine::value(copy), selecta::testing::safariApproved());

    // An original without one gives nothing.
    const QString plain = dir.filePath(u"plain.flac"_s);
    const QString other = dir.filePath(u"other.flac"_s);
    ASSERT_TRUE(writeFile(plain, "audio") && writeFile(other, "audio"));
    EXPECT_TRUE(quarantine::copy(plain, other));
    EXPECT_TRUE(quarantine::value(other).isEmpty());
}

TEST_F(QuarantineTest, SaysWhyAFileCantBeMarked)
{
    QString error;
    EXPECT_FALSE(quarantine::mark(dir.filePath(u"missing.mp3"_s), &error));
    EXPECT_TRUE(error.contains(u"missing.mp3"_s)) << error.toStdString();

    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may change any file";
    }
    // A file Selecta may not change: refused, and said so; the rest of its folder is still marked.
    const QString folder = dir.filePath(u"album"_s);
    const QString locked = folder + u"/01.mp3"_s;
    const QString open = folder + u"/02.mp3"_s;
    ASSERT_TRUE(writeFile(locked, "x") && writeFile(open, "x"));
    ASSERT_TRUE(makeReadOnly(locked));
    error.clear();
    EXPECT_FALSE(quarantine::mark(locked, &error));
    EXPECT_TRUE(error.contains(u"01.mp3"_s)) << error.toStdString();
    EXPECT_TRUE(quarantine::value(locked).isEmpty());
    error.clear();
    EXPECT_FALSE(quarantine::markTree(folder, &error));
    EXPECT_TRUE(error.contains(u"01.mp3"_s)) << error.toStdString();
    EXPECT_EQ(quarantine::agentOf(folder), u"Selecta"_s);
    EXPECT_EQ(quarantine::agentOf(open), u"Selecta"_s);
    const QString copyTarget = dir.filePath(u"locked-copy.mp3"_s);
    ASSERT_TRUE(writeFile(copyTarget, "x") && makeReadOnly(copyTarget));
    EXPECT_FALSE(quarantine::copy(open, copyTarget, &error));
}

TEST_F(QuarantineTest, NeverFollowsALink)
{
    const QString outside = dir.filePath(u"elsewhere/file.mp3"_s);
    const QString folder = dir.filePath(u"album"_s);
    ASSERT_TRUE(writeFile(outside, "x") && writeFile(folder + u"/01.mp3"_s, "x"));
    ASSERT_TRUE(QFile::link(outside, folder + u"/link.mp3"_s));
    EXPECT_TRUE(quarantine::markTree(folder));
    EXPECT_TRUE(quarantine::value(outside).isEmpty()); // what a link points to is never marked
    quarantine::mark(folder + u"/link.mp3"_s);         // at most the link itself
    EXPECT_TRUE(quarantine::value(outside).isEmpty());
}
