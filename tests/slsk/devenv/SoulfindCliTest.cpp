// SPDX-License-Identifier: GPL-3.0-or-later
// Two selecta-cli instances against a real Soulfind (contract acceptance 3): one shares a folder and answers, the other
// searches and downloads, over the direct path and over the forced indirect one. Opt-in like SoulfindTest.cpp:
// built with SELECTA_SOULFIND_TESTS=ON, run with SELECTA_DEVENV=1.
#include "devenv/Soulfind.h"
#include "fixtures/TestUtil.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::slsk::testing::devenvEnabled;
using selecta::slsk::testing::freePort;
using selecta::slsk::testing::Soulfind;
using selecta::slsk::testing::soulfindBinary;
using selecta::slsk::testing::waitFor;

namespace {

struct CliResult
{
    int exitCode = -1;
    QString out;
    QString err;
};

class SoulfindCliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!devenvEnabled()) {
            GTEST_SKIP() << "against a real Soulfind only with SELECTA_DEVENV=1 (devenv/README.md)";
        }
        const QString binary = soulfindBinary();
        ASSERT_FALSE(binary.isEmpty()) << "run devenv/fetch-soulfind.sh first, or set SELECTA_SOULFIND";
        const QString error = server.start(binary);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
        ASSERT_TRUE(folder.isValid());
        shared = folder.filePath(u"Shared"_s);
        ASSERT_TRUE(QDir().mkpath(shared + u"/Artist"_s));
        content = QByteArray(500'000, Qt::Uninitialized);
        QRandomGenerator random(21);
        for (char &c : content) {
            c = static_cast<char>(random.bounded(256));
        }
        QFile file(shared + u"/Artist/01 - Tidewater (Original Mix).flac"_s);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(content), content.size());
    }

    void TearDown() override
    {
        if (sharer && sharer->state() != QProcess::NotRunning) {
            sharer->terminate();
            sharer->waitForFinished(5000);
        }
    }

    QStringList options(const QString &user) const
    {
        return {u"--server"_s, u"127.0.0.1:%1"_s.arg(server.port()), u"--user"_s, user, u"--timeout"_s, u"15"_s};
    }

    static CliResult cli(const QStringList &arguments)
    {
        QProcess process;
        process.start(QStringLiteral(SELECTA_CLI_PATH), arguments);
        CliResult result;
        if (!process.waitForStarted(5000)) {
            result.err = process.errorString();
            return result;
        }
        if (!process.waitForFinished(120'000)) {
            process.kill();
            process.waitForFinished(3000);
        }
        result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
        result.out = QString::fromUtf8(process.readAllStandardOutput());
        result.err = QString::fromUtf8(process.readAllStandardError());
        return result;
    }

    // Starts `user` sharing the folder, telling the server `advertisedPort` if not 0, and waits until it's online.
    void startSharer(const QString &user, quint16 advertisedPort = 0)
    {
        sharer = std::make_unique<QProcess>();
        QStringList arguments =
            QStringList{u"slsk-share"_s} + options(user) + QStringList{u"--dir"_s, shared, u"--for"_s, u"120"_s};
        if (advertisedPort != 0) {
            arguments += QStringList{u"--advertise-port"_s, QString::number(advertisedPort)};
        }
        sharer->start(QStringLiteral(SELECTA_CLI_PATH), arguments);
        ASSERT_TRUE(sharer->waitForStarted(5000));
        ASSERT_TRUE(waitFor(
            [this] {
                sharerOut += QString::fromUtf8(sharer->readAllStandardOutput());
                return sharerOut.contains(u"sharing 1 files"_s) || sharer->state() == QProcess::NotRunning;
            },
            20s));
        ASSERT_TRUE(sharerOut.contains(u"sharing 1 files"_s))
            << sharerOut.toStdString() << sharer->readAllStandardError().toStdString();
    }

    // The seeker finds the file and downloads it; the bytes match.
    void searchAndDownload(const QString &sharerName)
    {
        const CliResult search = cli(QStringList{u"slsk-search"_s} + options(u"seeker"_s) +
                                     QStringList{u"--wait"_s, u"4"_s, u"tidewater"_s});
        ASSERT_EQ(search.exitCode, 0) << search.err.toStdString();
        const QString path = u"Shared\\Artist\\01 - Tidewater (Original Mix).flac"_s;
        ASSERT_TRUE(search.out.contains(sharerName + u'\t') && search.out.contains(path))
            << search.out.toStdString() << search.err.toStdString();
        const QString target = folder.filePath(u"downloaded-%1.flac"_s.arg(sharerName));
        const CliResult download = cli(QStringList{u"slsk-download"_s} + options(u"seeker"_s) +
                                       QStringList{u"--to"_s, target, sharerName, path});
        ASSERT_EQ(download.exitCode, 0) << download.out.toStdString() << download.err.toStdString();
        QFile result(target);
        ASSERT_TRUE(result.open(QIODevice::ReadOnly));
        EXPECT_EQ(result.readAll(), content);
    }

    Soulfind server;
    QTemporaryDir folder;
    QString shared;
    QByteArray content;
    std::unique_ptr<QProcess> sharer;
    QString sharerOut;
};

} // namespace

TEST_F(SoulfindCliTest, OneSharesTheOtherSearchesAndDownloadsDirectly)
{
    startSharer(u"cli-sharer"_s);
    ASSERT_FALSE(HasFatalFailure());
    searchAndDownload(u"cli-sharer"_s);
}

TEST_F(SoulfindCliTest, DownloadThroughTheIndirectPath)
{
    // The sharer advertises a port nobody listens on: the seeker's direct connection is refused and the sharer
    // dials back through the server.
    startSharer(u"cli-firewalled"_s, freePort());
    ASSERT_FALSE(HasFatalFailure());
    searchAndDownload(u"cli-firewalled"_s);
}
