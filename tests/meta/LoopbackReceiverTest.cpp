// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/LoopbackReceiver.h"

#include "meta/support/Await.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace selecta::meta;
using selecta::meta::detail::LoopbackReceiver;
using selecta::meta::testing::spinUntil;

namespace {

struct HttpAnswer
{
    int status = 0;
    QByteArray raw;
};

// A tiny HTTP client: connects to 127.0.0.1:port, sends `request` (in `parts` pieces), reads until the server closes.
HttpAnswer sendRaw(quint16 port, const QByteArray &request, int parts = 1)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    if (!spinUntil([&] { return socket.state() == QAbstractSocket::ConnectedState; }, 3000)) {
        return {};
    }
    const qsizetype step = std::max<qsizetype>(1, request.size() / parts);
    for (qsizetype offset = 0; offset < request.size(); offset += step) {
        socket.write(request.mid(offset, step));
        socket.flush();
        QTest::qWait(5);
    }
    HttpAnswer answer;
    spinUntil(
        [&] {
            answer.raw += socket.readAll();
            return socket.state() == QAbstractSocket::UnconnectedState;
        },
        3000);
    answer.raw += socket.readAll();
    const QList<QByteArray> statusLine = answer.raw.left(answer.raw.indexOf("\r\n")).split(' ');
    if (statusLine.size() >= 2) {
        answer.status = statusLine.at(1).toInt();
    }
    return answer;
}

QByteArray get(const QByteArray &target)
{
    return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: test\r\n\r\n";
}

class LoopbackReceiverTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        receiver = std::make_unique<LoopbackReceiver>("STATE123");
        ASSERT_TRUE(receiver->listen(0).ok());
        codes = std::make_unique<QSignalSpy>(receiver.get(), &LoopbackReceiver::codeReceived);
        failures = std::make_unique<QSignalSpy>(receiver.get(), &LoopbackReceiver::failed);
    }

    MetaError failure() const { return failures->first().first().value<MetaError>(); }

    std::unique_ptr<LoopbackReceiver> receiver;
    std::unique_ptr<QSignalSpy> codes;
    std::unique_ptr<QSignalSpy> failures;
};

} // namespace

TEST_F(LoopbackReceiverTest, ListensOnLoopbackWithACallbackUri)
{
    EXPECT_TRUE(receiver->isListening());
    EXPECT_NE(receiver->port(), 0);
    const QUrl uri = receiver->redirectUri();
    EXPECT_EQ(uri.scheme(), u"http"_s);
    EXPECT_EQ(uri.host(), u"127.0.0.1"_s);
    EXPECT_EQ(uri.port(), receiver->port());
    EXPECT_EQ(uri.path(), u"/callback"_s);
}

TEST_F(LoopbackReceiverTest, AcceptsTheCodeWhenTheStateMatches)
{
    const HttpAnswer answer = sendRaw(receiver->port(), get("/callback?code=AQB-code_1&state=STATE123"));
    EXPECT_EQ(answer.status, 200);
    EXPECT_TRUE(answer.raw.contains("You can close this tab"));
    EXPECT_TRUE(answer.raw.contains("Cache-Control: no-store"));
    ASSERT_EQ(codes->size(), 1);
    EXPECT_EQ(codes->first().first().toByteArray(), "AQB-code_1");
    EXPECT_EQ(failures->size(), 0);
    EXPECT_FALSE(receiver->isListening()); // one shot
}

TEST_F(LoopbackReceiverTest, WrongStateFailsTheSignIn)
{
    const HttpAnswer answer = sendRaw(receiver->port(), get("/callback?code=abc&state=OTHER"));
    EXPECT_EQ(answer.status, 400);
    ASSERT_EQ(failures->size(), 1);
    EXPECT_EQ(failure().code, ErrorCode::StateMismatch);
    EXPECT_EQ(codes->size(), 0);
    EXPECT_FALSE(receiver->isListening());
}

TEST_F(LoopbackReceiverTest, MissingStateFailsTheSignIn)
{
    sendRaw(receiver->port(), get("/callback?code=abc"));
    ASSERT_EQ(failures->size(), 1);
    EXPECT_EQ(failure().code, ErrorCode::StateMismatch);
}

TEST_F(LoopbackReceiverTest, UserDeniedIsAccessDenied)
{
    const HttpAnswer answer = sendRaw(receiver->port(), get("/callback?error=access_denied&state=STATE123"));
    EXPECT_EQ(answer.status, 200);
    ASSERT_EQ(failures->size(), 1);
    EXPECT_EQ(failure().code, ErrorCode::AccessDenied);
}

TEST_F(LoopbackReceiverTest, OtherErrorsAreNotEchoedBack)
{
    const HttpAnswer answer =
        sendRaw(receiver->port(), get("/callback?error=%3Cscript%3Ealert(1)%3C/script%3E&state=STATE123"));
    EXPECT_FALSE(answer.raw.contains("<script>"));
    EXPECT_FALSE(answer.raw.contains("alert"));
    ASSERT_EQ(failures->size(), 1);
    EXPECT_EQ(failure().code, ErrorCode::BadResponse);
    EXPECT_FALSE(failure().message.contains(u"<"_s));
}

TEST_F(LoopbackReceiverTest, StateMismatchPageDoesNotEchoTheRequest)
{
    const HttpAnswer answer = sendRaw(receiver->port(), get("/callback?code=%3Cb%3Ex&state=%3Cimg%3E"));
    EXPECT_FALSE(answer.raw.contains("<img>"));
    EXPECT_FALSE(answer.raw.contains("<b>x"));
}

TEST_F(LoopbackReceiverTest, MissingOrHugeCodeIsABadResponse)
{
    sendRaw(receiver->port(), get("/callback?state=STATE123"));
    ASSERT_EQ(failures->size(), 1);
    EXPECT_EQ(failure().code, ErrorCode::BadResponse);

    LoopbackReceiver other("S");
    ASSERT_TRUE(other.listen(0).ok());
    QSignalSpy otherFailures(&other, &LoopbackReceiver::failed);
    const QByteArray hugeCode(LoopbackReceiver::kMaxCodeLength + 1, 'c');
    sendRaw(other.port(), get("/callback?state=S&code=" + hugeCode));
    ASSERT_EQ(otherFailures.size(), 1);
    EXPECT_EQ(otherFailures.first().first().value<MetaError>().code, ErrorCode::BadResponse);
}

TEST_F(LoopbackReceiverTest, OtherPathsGet404AndTheListenerKeepsWaiting)
{
    EXPECT_EQ(sendRaw(receiver->port(), get("/favicon.ico")).status, 404);
    EXPECT_EQ(sendRaw(receiver->port(), get("/")).status, 404);
    EXPECT_TRUE(receiver->isListening());
    EXPECT_EQ(failures->size(), 0);
    EXPECT_EQ(sendRaw(receiver->port(), get("/callback?code=ok&state=STATE123")).status, 200);
    EXPECT_EQ(codes->size(), 1);
}

TEST_F(LoopbackReceiverTest, NonGetMethodsGet405)
{
    const QByteArray post = "POST /callback?code=x&state=STATE123 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    EXPECT_EQ(sendRaw(receiver->port(), post).status, 405);
    EXPECT_TRUE(receiver->isListening());
    EXPECT_EQ(codes->size(), 0);
}

TEST_F(LoopbackReceiverTest, GarbageGets400AndKeepsWaiting)
{
    EXPECT_EQ(sendRaw(receiver->port(), "HELLO\r\n\r\n").status, 400);
    EXPECT_TRUE(receiver->isListening());
}

TEST_F(LoopbackReceiverTest, RequestSplitAcrossWritesIsReassembled)
{
    const HttpAnswer answer = sendRaw(receiver->port(), get("/callback?code=split&state=STATE123"), 7);
    EXPECT_EQ(answer.status, 200);
    ASSERT_EQ(codes->size(), 1);
    EXPECT_EQ(codes->first().first().toByteArray(), "split");
}

TEST_F(LoopbackReceiverTest, OversizedRequestHeadIsRefused)
{
    QByteArray request = "GET /callback?code=x&state=STATE123 HTTP/1.1\r\nX-Filler: ";
    request += QByteArray(LoopbackReceiver::kMaxRequestBytes, 'f');
    request += "\r\n\r\n";
    EXPECT_EQ(sendRaw(receiver->port(), request).status, 431);
    EXPECT_EQ(codes->size(), 0);
    EXPECT_TRUE(receiver->isListening());
}

TEST_F(LoopbackReceiverTest, SpeculativeSilentConnectionsDontBlockTheRealOne)
{
    QTcpSocket idle1;
    QTcpSocket idle2;
    idle1.connectToHost(QHostAddress(QHostAddress::LocalHost), receiver->port());
    idle2.connectToHost(QHostAddress(QHostAddress::LocalHost), receiver->port());
    ASSERT_TRUE(spinUntil([&] {
        return idle1.state() == QAbstractSocket::ConnectedState && idle2.state() == QAbstractSocket::ConnectedState;
    }));
    EXPECT_EQ(sendRaw(receiver->port(), get("/callback?code=real&state=STATE123")).status, 200);
    EXPECT_EQ(codes->size(), 1);
}

TEST_F(LoopbackReceiverTest, ASecondCallbackAfterTheFirstIsTurnedAway)
{
    QTcpSocket early;
    early.connectToHost(QHostAddress(QHostAddress::LocalHost), receiver->port());
    ASSERT_TRUE(spinUntil([&] { return early.state() == QAbstractSocket::ConnectedState; }));
    QTest::qWait(20); // the receiver has accepted it
    EXPECT_EQ(sendRaw(receiver->port(), get("/callback?code=one&state=STATE123")).status, 200);
    early.write(get("/callback?code=two&state=STATE123"));
    early.flush();
    QTest::qWait(50);
    EXPECT_EQ(codes->size(), 1);
    EXPECT_EQ(failures->size(), 0);
}

TEST(LoopbackReceiverPort, PortInUseIsReported)
{
    QTcpServer occupier;
    ASSERT_TRUE(occupier.listen(QHostAddress(QHostAddress::LocalHost), 0));
    LoopbackReceiver receiver("S");
    const Result<void> result = receiver.listen(occupier.serverPort());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::PortUnavailable);
    EXPECT_FALSE(receiver.isListening());
}

TEST(LoopbackReceiverPort, CloseStopsListeningSilently)
{
    LoopbackReceiver receiver("S");
    ASSERT_TRUE(receiver.listen(0).ok());
    const QSignalSpy codes(&receiver, &LoopbackReceiver::codeReceived);
    const QSignalSpy failures(&receiver, &LoopbackReceiver::failed);
    const quint16 port = receiver.port();
    receiver.close();
    EXPECT_FALSE(receiver.isListening());
    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    spinUntil([&] { return socket.state() == QAbstractSocket::UnconnectedState; }, 500);
    EXPECT_EQ(codes.size(), 0);
    EXPECT_EQ(failures.size(), 0);
}
