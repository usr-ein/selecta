// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli's slsk-* commands (tools/selecta-cli/commands/slsk/), run as processes against the in-process fakes on
// 127.0.0.1.
#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/Samples.h"
#include "fixtures/TestUtil.h"

#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using namespace std::chrono_literals;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;

namespace {

struct CliRun
{
    int exitCode = -1;
    QString out;
    QString err;
};

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator random(seed);
    for (char &c : bytes) {
        c = static_cast<char>(random.bounded(256));
    }
    return bytes;
}

class CliTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(server.start()); }

    QStringList serverOptions(const QString &user) const
    {
        return {u"--server"_s, u"127.0.0.1:%1"_s.arg(server.port()), u"--user"_s, user, u"--timeout"_s, u"10"_s};
    }

    // Runs selecta-cli to the end while this process keeps serving (the fakes need the event loop).
    static CliRun cli(const QStringList &arguments, std::chrono::milliseconds limit = 30s)
    {
        QProcess process;
        process.start(QStringLiteral(SELECTA_CLI_PATH), arguments);
        CliRun run;
        if (!process.waitForStarted(5000)) {
            run.err = process.errorString();
            return run;
        }
        if (!waitFor([&process] { return process.state() == QProcess::NotRunning; }, limit)) {
            process.kill();
            process.waitForFinished(3000);
        }
        run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
        run.out = QString::fromUtf8(process.readAllStandardOutput());
        run.err = QString::fromUtf8(process.readAllStandardError());
        return run;
    }

    std::unique_ptr<FakePeer> makePeer(const QString &name)
    {
        auto peer = std::make_unique<FakePeer>(name, &server);
        peer->answerIndirect = false;
        EXPECT_TRUE(peer->start());
        return peer;
    }

    FakeServer server;
};

} // namespace

TEST_F(CliTest, RefusesAnythingButATestServer)
{
    // Public addresses and host names never get a connection (names aren't even resolved).
    const CliRun publicAddress = cli({u"slsk-login"_s, u"--server"_s, u"203.0.113.5:2242"_s, u"--user"_s, u"x"_s});
    EXPECT_EQ(publicAddress.exitCode, 2);
    EXPECT_TRUE(publicAddress.err.contains(u"live Soulseek network"_s)) << publicAddress.err.toStdString();
    const CliRun name = cli({u"slsk-login"_s, u"--server"_s, u"example.org:2242"_s, u"--user"_s, u"x"_s});
    EXPECT_EQ(name.exitCode, 2);
    EXPECT_TRUE(name.err.contains(u"never resolves"_s)) << name.err.toStdString();
    EXPECT_EQ(server.connectionCount(), 0);
}

TEST_F(CliTest, LogsInAndReportsAWrongPassword)
{
    const CliRun ok = cli(QStringList{u"slsk-login"_s} + serverOptions(u"cli-user"_s));
    EXPECT_EQ(ok.exitCode, 0) << ok.err.toStdString();
    EXPECT_TRUE(ok.out.contains(u"logged in as cli-user"_s)) << ok.out.toStdString();
    server.setAccount(u"cli-user"_s, u"right"_s);
    const CliRun wrong =
        cli(QStringList{u"slsk-login"_s} + serverOptions(u"cli-user"_s) + QStringList{u"--password"_s, u"wrong"_s});
    EXPECT_EQ(wrong.exitCode, 1);
    EXPECT_TRUE(wrong.err.contains(u"INVALIDPASS"_s)) << wrong.err.toStdString();
}

TEST_F(CliTest, SearchPrintsEachResult)
{
    const auto alice = makePeer(u"alice"_s);
    alice->shares = selecta::slsk::testing::sampleShareList(2, 2);
    alice->searchHits = {u"album"_s};
    QStringList queries;
    const QObject receiver;
    QObject::connect(&server, &FakeServer::messageReceived, &receiver,
                     [sent = &queries](const QString &, const ToServerMessage &message) {
                         if (const auto *search = std::get_if<toserver::FileSearch>(&message)) {
                             sent->append(search->query);
                         }
                     });
    const CliRun run =
        cli(QStringList{u"slsk-search"_s} + serverOptions(u"seeker"_s) + QStringList{u"--wait"_s, u"2"_s, u"album"_s});
    EXPECT_EQ(run.exitCode, 0) << run.err.toStdString();
    const QStringList lines = run.out.split(u'\n', Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 4) << run.out.toStdString(); // 2 folders × 2 files
    EXPECT_TRUE(lines.at(0).startsWith(u"alice\tfree\t"_s));
    EXPECT_TRUE(lines.at(0).contains(u"Collection\\Artist 0\\Album (2020)\\"_s)) << lines.at(0).toStdString();
    EXPECT_TRUE(run.err.contains(u"4 files from 1 users"_s)) << run.err.toStdString();
    EXPECT_EQ(queries, QStringList{u"album"_s}); // as the governor cleaned it
}

TEST_F(CliTest, BrowseListsTheShares)
{
    const auto alice = makePeer(u"alice"_s);
    alice->shares = selecta::slsk::testing::sampleShareList(2, 3);
    const CliRun run = cli(QStringList{u"slsk-browse"_s} + serverOptions(u"browser"_s) + QStringList{u"alice"_s});
    EXPECT_EQ(run.exitCode, 0) << run.err.toStdString();
    EXPECT_TRUE(run.out.contains(u"Collection\\Artist 1\\Album (2020)\n"_s)) << run.out.toStdString();
    EXPECT_EQ(run.out.count(u'\t') / 3, 6); // six file lines of three fields
    EXPECT_TRUE(run.err.contains(u"alice shares 6 files in 3 folders"_s)) << run.err.toStdString(); // one empty
}

TEST_F(CliTest, DownloadWritesTheFileAndResumes)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"Music\\Album\\01 Track.flac"_s);
    const QByteArray content = randomBytes(300'000, 5);
    alice->files.insert(path.raw, FakePeer::Upload{content, -1, false, {}});
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(u"track.flac"_s);
    {
        // Half of it is already here.
        QFile partial(target);
        ASSERT_TRUE(partial.open(QIODevice::WriteOnly));
        ASSERT_EQ(partial.write(content.left(100'000)), 100'000);
    }
    const CliRun run = cli(QStringList{u"slsk-download"_s} + serverOptions(u"getter"_s) +
                           QStringList{u"--to"_s, target, u"alice"_s, path.display()});
    EXPECT_EQ(run.exitCode, 0) << run.err.toStdString();
    EXPECT_TRUE(run.out.contains(u"done: "_s)) << run.out.toStdString();
    EXPECT_TRUE(run.out.contains(u"100000 resumed"_s)) << run.out.toStdString();
    QFile result(target);
    ASSERT_TRUE(result.open(QIODevice::ReadOnly));
    EXPECT_EQ(result.readAll(), content);
    // A refusal is reported with the uploader's words.
    alice->queueBehaviour = FakePeer::QueueBehaviour::Deny;
    alice->denyReason = u"Too many files"_s;
    const CliRun refused = cli(QStringList{u"slsk-download"_s} + serverOptions(u"getter"_s) +
                               QStringList{u"--to"_s, dir.filePath(u"other.flac"_s), u"alice"_s, path.display()});
    EXPECT_EQ(refused.exitCode, 1);
    EXPECT_TRUE(refused.err.contains(u"Too many files"_s)) << refused.err.toStdString();
}

TEST_F(CliTest, ShareAnswersSearchesAndUploads)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString shared = dir.filePath(u"Shared"_s);
    ASSERT_TRUE(QDir().mkpath(shared + u"/Kel Varo"_s));
    const QByteArray content = randomBytes(200'000, 6);
    {
        QFile file(shared + u"/Kel Varo/01 - Amber Engine.flac"_s);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(content), content.size());
    }
    QProcess sharer;
    sharer.start(QStringLiteral(SELECTA_CLI_PATH), QStringList{u"slsk-share"_s} + serverOptions(u"sharer"_s) +
                                                       QStringList{u"--dir"_s, shared, u"--for"_s, u"30"_s});
    ASSERT_TRUE(sharer.waitForStarted(5000));
    QString out;
    ASSERT_TRUE(waitFor(
        [&] {
            out += QString::fromUtf8(sharer.readAllStandardOutput());
            return out.contains(u"sharing 1 files in 1 folders as sharer"_s);
        },
        15s))
        << out.toStdString() << sharer.readAllStandardError().toStdString();
    ASSERT_TRUE(waitFor([&] {
        const FakeServer::Client *client = server.client(u"sharer"_s);
        return client != nullptr && client->listenPort != 0;
    }));
    // A search reaches it through the server; the answer comes to the searcher.
    const auto bob = makePeer(u"bob"_s);
    ASSERT_TRUE(server.sendTo(u"sharer"_s, fromserver::SearchRequest{u"bob"_s, 77, u"amber engine"_s}));
    ASSERT_TRUE(waitFor([&] { return bob->receivedOf<peer::FileSearchResponse>().size() == 1; }));
    const peer::FileSearchResponse answer = bob->receivedOf<peer::FileSearchResponse>().at(0);
    EXPECT_EQ(answer.token, 77U);
    ASSERT_EQ(answer.results.size(), 1);
    EXPECT_EQ(answer.results.at(0).name.display(), u"Shared\\Kel Varo\\01 - Amber Engine.flac"_s);
    // bob queues it; the sharer uploads it.
    const quint16 port = static_cast<quint16>(server.client(u"sharer"_s)->listenPort);
    const quint64 connection = bob->connectTo(u"sharer"_s, QHostAddress::LocalHost, port, ConnectionType::Peer);
    bob->send(connection, frame(peer::QueueUpload{answer.results.at(0).name}));
    ASSERT_TRUE(waitFor([&] { return bob->downloadedFiles.size() == 1; }, 15s));
    EXPECT_EQ(bob->downloadedFiles.at(0), content);
    ASSERT_TRUE(waitFor([&] {
        out += QString::fromUtf8(sharer.readAllStandardOutput());
        return out.contains(u": completed"_s);
    }));
    EXPECT_TRUE(out.contains(u"answered bob's search \"amber engine\" with 1 files"_s)) << out.toStdString();
    sharer.terminate();
    sharer.waitForFinished(5000);
}
