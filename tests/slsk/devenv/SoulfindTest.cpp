// SPDX-License-Identifier: GPL-3.0-or-later
// slsk against a real Soulfind on this machine (ADR-013, devenv/README.md). Built only with SELECTA_SOULFIND_TESTS=ON
// (CTest label `devenv`); every test skips unless SELECTA_DEVENV=1, and then needs the pinned build
// (devenv/fetch-soulfind.sh). Each test starts its own Soulfind on a free port with a throwaway database, and stops it
// afterwards. The clients connect to 127.0.0.1 and listen there only: Soulfind hands out this machine's LAN address,
// which PeerNetwork reaches over loopback.
#include "fixtures/TestPermits.h"
#include "fixtures/TestUtil.h"
#include "slsk/Client.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using namespace std::chrono_literals;
using selecta::governor::Operation;
using selecta::slsk::testing::permit;
using selecta::slsk::testing::TestPermits;
using selecta::slsk::testing::waitFor;

namespace {

// A port nobody listens on right now.
quint16 freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

QString soulfindBinary()
{
    if (const QString path = qEnvironmentVariable("SELECTA_SOULFIND"); !path.isEmpty()) {
        return path;
    }
    const QString cached = QStringLiteral(SELECTA_DEVENV_DIR) + u"/.cache/soulfind/current/soulfind"_s;
    return QFileInfo(cached).isExecutable() ? cached : QString();
}

// One test's server.
class Soulfind
{
public:
    Soulfind() = default;
    Soulfind(const Soulfind &) = delete;
    Soulfind &operator=(const Soulfind &) = delete;
    Soulfind(Soulfind &&) = delete;
    Soulfind &operator=(Soulfind &&) = delete;

    ~Soulfind()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.terminate();
            if (!m_process.waitForFinished(3000)) {
                m_process.kill();
                m_process.waitForFinished(3000);
            }
        }
        if (qEnvironmentVariable("SELECTA_SOULFIND_DEBUG") == u"1"_s) {
            qInfo().noquote() << "--- Soulfind's output ---\n" << QString::fromLocal8Bit(m_process.readAll());
        }
    }

    // Starts it and waits until it accepts connections. Empty on success, else why not.
    QString start(const QString &binary)
    {
        m_port = freePort();
        if (m_port == 0 || !m_data.isValid()) {
            return u"no free port or temporary folder"_s;
        }
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_MACOS
        // As devenv/run-soulfind.sh: Soulfind needs Homebrew's SQLite on macOS.
        for (const QString &lib : {u"/opt/homebrew/opt/sqlite/lib"_s, u"/usr/local/opt/sqlite/lib"_s}) {
            if (QFileInfo(lib).isDir()) {
                environment.insert(u"DYLD_LIBRARY_PATH"_s, lib);
                break;
            }
        }
#endif
        m_process.setProcessEnvironment(environment);
        m_process.setProcessChannelMode(QProcess::MergedChannels);
        QStringList arguments{u"--port"_s, QString::number(m_port), u"--database"_s, m_data.filePath(u"soulfind.db"_s)};
        if (qEnvironmentVariable("SELECTA_SOULFIND_DEBUG") == u"1"_s) {
            arguments.append(u"--debug"_s);
        }
        m_process.start(binary, arguments);
        if (!m_process.waitForStarted(5000)) {
            return u"couldn't start %1: %2"_s.arg(binary, m_process.errorString());
        }
        const bool up = waitFor(
            [this] {
                QTcpSocket probe;
                probe.connectToHost(QHostAddress::LocalHost, m_port);
                return probe.waitForConnected(200);
            },
            10s);
        if (!up) {
            return u"Soulfind didn't listen: %1"_s.arg(QString::fromLocal8Bit(m_process.readAll()));
        }
        return {};
    }

    quint16 port() const { return m_port; }

private:
    QProcess m_process;
    QTemporaryDir m_data;
    quint16 m_port = 0;
};

// A whole client stack logged in to the test server, sharing at most one file.
struct Node
{
    TestPermits permits;
    std::unique_ptr<Client> client;
    QByteArray file;
    RemotePath path;
    std::unique_ptr<QBuffer> source;

    Node()
        : client(std::make_unique<Client>(permits))
    {}

    // Logs in as `name`; `advertisedPort` overrides the listening port the server is told (0: the real one).
    bool logIn(quint16 serverPort, const QString &name, quint16 advertisedPort = 0) const
    {
        Client &c = *client;
        c.server().setTimeouts({5s, 5s, 5min});
        if (!c.peers().listen(0, QHostAddress(QHostAddress::LocalHost))) {
            return false;
        }
        c.server().setListenPort(advertisedPort != 0 ? advertisedPort : c.peers().listenPort());
        if (!c.server().connectToServer(QHostAddress(QHostAddress::LocalHost), serverPort, permit(Operation::Login)) ||
            !c.server().login(name, u"devenv-"_s + name)) {
            return false;
        }
        return waitFor([&c] { return c.server().isLoggedIn(); }, 10s);
    }

    // Shares `bytes` as `virtualPath`: answers searches for its words and uploads it when asked.
    void share(const QString &virtualPath, const QByteArray &bytes)
    {
        file = bytes;
        path = RemotePath::fromText(virtualPath);
        Client *c = client.get();
        QObject::connect(c, &Client::searchRequestReceived, c, [this, c](const IncomingSearch &search) {
            if (search.username == c->server().username() ||
                !path.display().contains(search.query, Qt::CaseInsensitive)) {
                return;
            }
            const FileEntry entry{path, static_cast<quint64>(file.size()), u"flac"_s, {}};
            c->answerSearch(search, {entry}, Client::UploadAvailability{true, 1'000'000, 0});
        });
        QObject::connect(&c->transfers(), &TransferManager::uploadRequested, c,
                         [this, c](const QString &user, const RemotePath &requested, bool /*legacy*/) {
                             if (requested != path) {
                                 c->transfers().denyUpload(user, requested, u"File not shared."_s);
                                 return;
                             }
                             source = std::make_unique<QBuffer>();
                             source->setData(file);
                             source->open(QIODevice::ReadOnly);
                             c->transfers().startUpload(
                                 UploadRequest{user, requested, static_cast<quint64>(file.size())}, source.get());
                         });
    }
};

class MemoryStorage : public DownloadStorage
{
public:
    QIODevice *openDownload(const DownloadInfo & /*download*/) override
    {
        m_device = std::make_unique<QBuffer>(&contents);
        return m_device->open(QIODevice::WriteOnly | QIODevice::Append) ? m_device.get() : nullptr;
    }
    void closeDownload(const DownloadInfo & /*download*/, QIODevice *device) override { device->close(); }
    QByteArray contents;

private:
    std::unique_ptr<QBuffer> m_device;
};

class SoulfindTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (qEnvironmentVariable("SELECTA_DEVENV") != u"1"_s) {
            GTEST_SKIP() << "against a real Soulfind only with SELECTA_DEVENV=1 (devenv/README.md)";
        }
        const QString binary = soulfindBinary();
        ASSERT_FALSE(binary.isEmpty()) << "run devenv/fetch-soulfind.sh first, or set SELECTA_SOULFIND";
        const QString error = server.start(binary);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    }

    static QByteArray randomFile(qsizetype size)
    {
        QByteArray bytes(size, Qt::Uninitialized);
        QRandomGenerator random(11);
        for (char &c : bytes) {
            c = static_cast<char>(random.bounded(256));
        }
        return bytes;
    }

    // `downloader` finds the shared file by searching, downloads it, and gets the same bytes.
    void searchAndDownload(Node &downloader, const Node &uploader)
    {
        MemoryStorage storage;
        downloader.client->transfers().setDownloadStorage(&storage);
        const QSignalSpy results(&downloader.client->peers(), &PeerNetwork::searchResponseReceived);
        const quint32 token = downloader.client->search(u"tidewater"_s, permit(Operation::Search));
        ASSERT_NE(token, 0U);
        ASSERT_TRUE(waitFor([&] { return !results.isEmpty(); }, 15s)) << "no search results";
        const auto response = results.at(0).at(1).value<peer::FileSearchResponse>();
        ASSERT_EQ(results.at(0).at(0).toString(), uploader.client->server().username());
        ASSERT_EQ(response.results.size(), 1);
        EXPECT_EQ(response.results.at(0).name, uploader.path);
        downloader.client->stopSearch(token);

        const QSignalSpy changes(&downloader.client->transfers(), &TransferManager::downloadChanged);
        const TransferId id = downloader.client->transfers().download(
            DownloadRequest{uploader.client->server().username(), response.results.at(0).name,
                            response.results.at(0).size, false},
            permit(Operation::QueueDownload));
        ASSERT_NE(id, 0U);
        ASSERT_TRUE(waitFor(
            [&] { return !changes.isEmpty() && changes.constLast().at(0).value<DownloadInfo>().isTerminal(); }, 30s));
        const auto done = changes.constLast().at(0).value<DownloadInfo>();
        EXPECT_EQ(done.state, DownloadState::Completed)
            << transferErrorName(done.error) << " " << done.detail.toStdString();
        EXPECT_EQ(storage.contents, uploader.file);
        downloader.client->transfers().setDownloadStorage(nullptr);
    }

    Soulfind server;
};

} // namespace

TEST_F(SoulfindTest, LoginReloginAndAWrongPassword)
{
    Node first;
    ASSERT_TRUE(first.logIn(server.port(), u"devenv-one"_s));
    // The same name again pushes the first session out.
    const Node second;
    const QSignalSpy ended(&first.client->server(), &ServerSession::ended);
    ASSERT_TRUE(second.logIn(server.port(), u"devenv-one"_s));
    ASSERT_TRUE(waitFor([&] { return !ended.isEmpty(); }, 10s));
    EXPECT_EQ(first.client->server().endReason(), ServerSession::EndReason::Relogged);
    // Soulfind registers a name at its first login; another password is refused.
    ServerSession wrong;
    const QSignalSpy refused(&wrong, &ServerSession::ended);
    ASSERT_TRUE(wrong.connectToServer(QHostAddress(QHostAddress::LocalHost), server.port(), permit(Operation::Login)));
    ASSERT_TRUE(wrong.login(u"devenv-one"_s, u"not the password"_s));
    ASSERT_TRUE(waitFor([&] { return !refused.isEmpty(); }, 10s));
    EXPECT_EQ(wrong.endReason(), ServerSession::EndReason::LoginRejected);
    EXPECT_EQ(wrong.loginRejection().reason, u"INVALIDPASS"_s);
}

TEST_F(SoulfindTest, SearchAnswerAndDownloadOverTheDirectPath)
{
    Node uploader;
    Node downloader;
    uploader.share(u"Collection\\Artist\\01 - Tidewater (Original Mix).flac"_s, randomFile(700'000));
    ASSERT_TRUE(uploader.logIn(server.port(), u"devenv-uploader"_s));
    ASSERT_TRUE(downloader.logIn(server.port(), u"devenv-downloader"_s));
    searchAndDownload(downloader, uploader);
}

TEST_F(SoulfindTest, SearchAnswerAndDownloadOverTheIndirectPath)
{
    // The uploader tells the server a port nobody listens on: the downloader's direct connection fails, and the
    // uploader dials back through the server's ConnectToPeer (PierceFireWall).
    Node uploader;
    Node downloader;
    uploader.share(u"Collection\\Artist\\02 - Tidewater (Dub).flac"_s, randomFile(400'000));
    ASSERT_TRUE(uploader.logIn(server.port(), u"devenv-firewalled"_s, freePort()));
    ASSERT_TRUE(downloader.logIn(server.port(), u"devenv-downloader"_s));
    searchAndDownload(downloader, uploader);
}
