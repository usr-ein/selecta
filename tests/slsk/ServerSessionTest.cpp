// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/ServerSession.h"

#include "fixtures/FakeServer.h"
#include "fixtures/TestUtil.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using namespace std::chrono_literals;
using selecta::governor::Operation;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::permit;
using selecta::slsk::testing::settle;
using selecta::slsk::testing::waitFor;
using EndReason = ServerSession::EndReason;
using State = ServerSession::State;

namespace {

class ServerSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        session.setTimeouts({2s, 300ms, 5min});
    }

    bool connect(ServerSession &s)
    {
        return s.connectToServer(server.address(), server.port(), permit(Operation::Login));
    }

    // Connects and logs in as `username`; true once logged in.
    bool logIn(ServerSession &s, const QString &username = u"selecta"_s)
    {
        if (!connect(s) || !s.login(username, u"secret"_s)) {
            return false;
        }
        return waitFor([&] { return s.isLoggedIn(); });
    }

    // The session's end reason, once it has ended.
    std::optional<EndReason> waitForEnd(ServerSession &s)
    {
        if (!waitFor([&] { return s.state() == State::Disconnected && s.endReason() != EndReason::None; })) {
            return std::nullopt;
        }
        return s.endReason();
    }

    FakeServer server;
    ServerSession session;
};

} // namespace

// --- login -----------------------------------------------------------------------------------------------------------

TEST_F(ServerSessionTest, LogsInThenSendsTheBurst)
{
    const QSignalSpy loggedIn(&session, &ServerSession::loggedIn);
    session.setListenPort(2240);
    session.setSharedCounts(12, 345);
    ASSERT_TRUE(logIn(session));
    ASSERT_EQ(loggedIn.size(), 1);
    const auto info = loggedIn.at(0).at(0).value<LoginInfo>();
    EXPECT_EQ(info.username, u"selecta"_s);
    EXPECT_EQ(info.ownAddress, QHostAddress(QHostAddress::LocalHost));
    EXPECT_FALSE(info.greeting.isEmpty());

    ASSERT_TRUE(waitFor([&] { return server.received(u"selecta"_s).size() >= 6; }));
    const QList<ToServerMessage> burst = server.received(u"selecta"_s).mid(0, 6);
    EXPECT_EQ(std::get<toserver::SetWaitPort>(burst[0]).port, 2240U);
    EXPECT_FALSE(std::get<toserver::SetWaitPort>(burst[0]).obfuscationType.has_value());
    EXPECT_EQ(std::get<toserver::SetStatus>(burst[1]).status, 2);
    EXPECT_EQ(std::get<toserver::SharedFoldersFiles>(burst[2]).folders, 12U);
    EXPECT_EQ(std::get<toserver::SharedFoldersFiles>(burst[2]).files, 345U);
    EXPECT_TRUE(std::holds_alternative<toserver::CheckPrivileges>(burst[3]));
    EXPECT_EQ(std::get<toserver::WatchUser>(burst[4]).username, u"selecta"_s);
    EXPECT_FALSE(std::get<toserver::EnableRoomInvitations>(burst[5]).enable);
    // Nothing before the login reply.
    EXPECT_TRUE(server.client(u"selecta"_s)->codesBeforeLogin.isEmpty());
}

TEST_F(ServerSessionTest, LoginCanBeRequestedBeforeTheConnectionIsUp)
{
    ASSERT_TRUE(connect(session));
    EXPECT_EQ(session.state(), State::Connecting);
    ASSERT_TRUE(session.login(u"early"_s, u"pw"_s));
    EXPECT_TRUE(waitFor([&] { return session.isLoggedIn(); }));
}

TEST_F(ServerSessionTest, NothingElseGoesOutWhileTheLoginReplyIsPending)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::Silent);
    session.setTimeouts({2s, 2s, 5min});
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"waiting"_s, u"pw"_s));
    ASSERT_TRUE(waitFor([&] { return session.state() == State::LoggingIn; }));
    // Changes while waiting are kept for the burst, not sent now.
    session.setListenPort(1234);
    session.setStatus(UserStatus::Away);
    session.setSharedCounts(1, 2);
    EXPECT_FALSE(session.watchUser(u"x"_s));
    EXPECT_FALSE(session.fileSearch(1, u"q"_s, permit(Operation::Search)));
    settle();
    ASSERT_EQ(server.connectionCount(), 1);
    EXPECT_TRUE(server.loggedInUsers().isEmpty());
    EXPECT_TRUE(server.codesReceivedBeforeLogin().isEmpty());
}

TEST_F(ServerSessionTest, ConnectionWithoutLoginIsBounded)
{
    session.setTimeouts({2s, 150ms, 5min});
    ASSERT_TRUE(connect(session));
    EXPECT_EQ(waitForEnd(session), EndReason::LoggedOut);
}

TEST_F(ServerSessionTest, LoginWithoutAConnectionOrNameIsRefused)
{
    EXPECT_FALSE(session.login(u"x"_s, u"y"_s)); // not connected
    ASSERT_TRUE(connect(session));
    EXPECT_FALSE(session.login(QString(), u"y"_s));
    EXPECT_TRUE(session.login(u"x"_s, u"y"_s));
    EXPECT_FALSE(session.login(u"x"_s, u"y"_s)); // only once per session
}

TEST_F(ServerSessionTest, ConnectNeedsALoginPermitAndAnIdleSession)
{
    EXPECT_FALSE(session.connectToServer(server.address(), server.port(), permit(Operation::Search)));
    EXPECT_EQ(session.state(), State::Disconnected);
    ASSERT_TRUE(connect(session));
    EXPECT_FALSE(connect(session)); // already running
}

// --- terminal states -------------------------------------------------------------------------------------------------

TEST_F(ServerSessionTest, RejectedLoginKeepsTheReason)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::Reject, u"INVALIDPASS"_s);
    const QSignalSpy ended(&session, &ServerSession::ended);
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"bob"_s, u"wrong"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::LoginRejected);
    EXPECT_EQ(session.loginRejection().reason, u"INVALIDPASS"_s);
    ASSERT_EQ(ended.size(), 1); // exactly once
    EXPECT_EQ(ended.at(0).at(0).value<EndReason>(), EndReason::LoginRejected);
}

TEST_F(ServerSessionTest, InvalidUsernameCarriesTheDetail)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::Reject, u"INVALIDUSERNAME"_s, u"Nick too long."_s);
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"a-very-long-name"_s, u"pw"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::LoginRejected);
    EXPECT_EQ(session.loginRejection().detail, u"Nick too long."_s);
}

TEST_F(ServerSessionTest, ServerFullIsItsOwnReason)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::Reject, u"SVRFULL"_s);
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"bob"_s, u"pw"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::ServerFull);
}

TEST_F(ServerSessionTest, NoLoginReplyIsALoginTimeout)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::Silent);
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"banned"_s, u"pw"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::LoginTimeout);
}

TEST_F(ServerSessionTest, CloseOnLoginIsAnInstantEof)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::CloseOnLogin);
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"throttled"_s, u"pw"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::InstantEof);
}

TEST_F(ServerSessionTest, CloseOnConnectIsAnInstantEof)
{
    server.setLoginBehaviour(FakeServer::LoginBehaviour::CloseOnConnect);
    ASSERT_TRUE(connect(session));
    ASSERT_TRUE(session.login(u"throttled"_s, u"pw"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::InstantEof);
}

TEST_F(ServerSessionTest, RefusedConnectionIsConnectFailed)
{
    QTcpServer closed;
    ASSERT_TRUE(closed.listen(QHostAddress::LocalHost, 0));
    const quint16 port = closed.serverPort();
    closed.close(); // nothing listens there now
    ASSERT_TRUE(session.connectToServer(QHostAddress::LocalHost, port, permit(Operation::Login)));
    EXPECT_EQ(waitForEnd(session), EndReason::ConnectFailed);
}

TEST_F(ServerSessionTest, ConnectTimeoutIsConnectFailed)
{
    // A listener whose accept queue is full: further SYNs go unanswered, so the connection never completes.
    QTcpServer full;
    full.setListenBacklogSize(0);
    ASSERT_TRUE(full.listen(QHostAddress::LocalHost, 0));
    full.pauseAccepting();
    QList<QTcpSocket *> fillers;
    for (int i = 0; i < 8; ++i) {
        auto *filler = new QTcpSocket(&full);
        filler->connectToHost(QHostAddress::LocalHost, full.serverPort());
        fillers.append(filler);
    }
    settle(200ms);
    session.setTimeouts({250ms, 300ms, 5min});
    ASSERT_TRUE(session.connectToServer(QHostAddress::LocalHost, full.serverPort(), permit(Operation::Login)));
    ASSERT_TRUE(session.login(u"patient"_s, u"pw"_s));
    const std::optional<EndReason> reason = waitForEnd(session);
    if (reason == EndReason::LoginTimeout || reason == EndReason::InstantEof) {
        // macOS completes connections past a full backlog (Linux drops the SYN, so the timer fires there).
        GTEST_SKIP() << "this platform completes connections past a full backlog";
    }
    EXPECT_EQ(reason, EndReason::ConnectFailed);
}

TEST_F(ServerSessionTest, SecondLoginWithTheSameNameRelogsTheFirst)
{
    ServerSession second;
    second.setTimeouts({2s, 2s, 5min});
    ASSERT_TRUE(logIn(session, u"twin"_s));
    const QSignalSpy ended(&session, &ServerSession::ended);
    ASSERT_TRUE(logIn(second, u"twin"_s));
    EXPECT_EQ(waitForEnd(session), EndReason::Relogged);
    EXPECT_EQ(ended.size(), 1);
    EXPECT_TRUE(second.isLoggedIn());
}

TEST_F(ServerSessionTest, LogOutEndsAtOnceAndCanReconnect)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy ended(&session, &ServerSession::ended);
    session.disconnectFromServer();
    ASSERT_EQ(ended.size(), 1); // synchronous
    EXPECT_EQ(session.endReason(), EndReason::LoggedOut);
    EXPECT_EQ(session.state(), State::Disconnected);
    session.disconnectFromServer(); // harmless when already down
    EXPECT_EQ(ended.size(), 1);
    ASSERT_TRUE(logIn(session));
    EXPECT_EQ(session.endReason(), EndReason::None);
}

TEST_F(ServerSessionTest, DroppedSessionIsConnectionLost)
{
    ASSERT_TRUE(logIn(session));
    server.disconnectClient(u"selecta"_s);
    EXPECT_EQ(waitForEnd(session), EndReason::ConnectionLost);
}

TEST_F(ServerSessionTest, BanMessageIsRecognisedAndEndsTheSessionAsABan)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy notices(&session, &ServerSession::serverNotice);
    const QSignalSpy messages(&session, &ServerSession::privateMessageReceived);
    const QString text =
        u"System Message: You have been banned for 30 minutes. This is usually the result of doing too many "
        u"operations at once."_s;
    ASSERT_TRUE(server.sendPrivateMessage(u"server"_s, u"selecta"_s, text));
    ASSERT_TRUE(waitFor([&] { return notices.size() == 1; }));
    const auto notice = notices.at(0).at(0).value<ServerNotice>();
    EXPECT_EQ(notice.kind, ServerNotice::Kind::Ban);
    EXPECT_EQ(notice.source, ServerNotice::Source::PrivateMessage);
    EXPECT_EQ(notice.text, text);
    EXPECT_EQ(messages.size(), 1);
    server.disconnectClient(u"selecta"_s);
    EXPECT_EQ(waitForEnd(session), EndReason::BanNotice);
}

TEST_F(ServerSessionTest, BanTextInAnAdminMessageCountsToo)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy notices(&session, &ServerSession::serverNotice);
    ASSERT_TRUE(server.sendTo(u"selecta"_s, fromserver::AdminMessage{u"You have been banned for 30 minutes."_s}));
    ASSERT_TRUE(waitFor([&] { return notices.size() == 1; }));
    EXPECT_EQ(notices.at(0).at(0).value<ServerNotice>().kind, ServerNotice::Kind::Ban);
    EXPECT_EQ(notices.at(0).at(0).value<ServerNotice>().source, ServerNotice::Source::AdminMessage);
}

TEST_F(ServerSessionTest, RestrictedAndOrdinaryNotices)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy notices(&session, &ServerSession::serverNotice);
    server.sendPrivateMessage(u"server"_s, u"selecta"_s,
                              u"Your connection is restricted: You cannot search or chat. Your client version is too "
                              u"old."_s);
    server.sendTo(u"selecta"_s, fromserver::AdminMessage{u"Maintenance tonight"_s});
    server.sendPrivateMessage(u"friend"_s, u"selecta"_s, u"not a notice"_s);
    ASSERT_TRUE(waitFor([&] { return notices.size() == 2; }));
    settle();
    ASSERT_EQ(notices.size(), 2);
    EXPECT_EQ(notices.at(0).at(0).value<ServerNotice>().kind, ServerNotice::Kind::Restricted);
    EXPECT_EQ(notices.at(1).at(0).value<ServerNotice>().kind, ServerNotice::Kind::Other);
    EXPECT_EQ(session.endReason(), EndReason::None); // notices alone don't end anything
}

TEST_F(ServerSessionTest, OversizeFrameIsAProtocolViolation)
{
    ASSERT_TRUE(logIn(session));
    // Length 0x7FFFFFFF: far beyond the 16 MiB server cap.
    server.sendRawTo(u"selecta"_s, QByteArray::fromHex("ff ff ff 7f 40 00 00 00"));
    EXPECT_EQ(waitForEnd(session), EndReason::ProtocolViolation);
}

TEST_F(ServerSessionTest, ImpossibleFrameLengthIsAProtocolViolation)
{
    ASSERT_TRUE(logIn(session));
    server.sendRawTo(u"selecta"_s, QByteArray::fromHex("02 00 00 00 01 00"));
    EXPECT_EQ(waitForEnd(session), EndReason::ProtocolViolation);
}

TEST_F(ServerSessionTest, MalformedAndUnknownMessagesAreDroppedTheSessionGoesOn)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy interval(&session, &ServerSession::wishlistIntervalReceived);
    // A PossibleParents announcing far more candidates than it carries, an unknown code, an ignored ticker.
    server.sendRawTo(u"selecta"_s, encodeFrame(CodeWidth::Word, 102, QByteArray::fromHex("ff ff 00 00")));
    server.sendRawTo(u"selecta"_s, encodeFrame(CodeWidth::Word, 4242, "junk"));
    server.sendRawTo(u"selecta"_s, encodeFrame(CodeWidth::Word, 113, "tickers"));
    server.sendTo(u"selecta"_s, fromserver::WishlistInterval{120});
    ASSERT_TRUE(waitFor([&] { return interval.size() == 1; }));
    EXPECT_EQ(interval.at(0).at(0).toUInt(), 120U);
    EXPECT_TRUE(session.isLoggedIn());
}

TEST_F(ServerSessionTest, PingsWhileLoggedIn)
{
    session.setTimeouts({2s, 2s, 60ms});
    ASSERT_TRUE(logIn(session));
    EXPECT_TRUE(waitFor([&] { return server.receivedOf<toserver::ServerPing>(u"selecta"_s).size() >= 2; }));
}

// --- events ----------------------------------------------------------------------------------------------------------

TEST_F(ServerSessionTest, WelcomeMessagesBecomeSignals)
{
    FakeServer::Welcome welcome;
    welcome.wishlistInterval = 120;
    welcome.privilegedUsers = {u"donor"_s};
    welcome.excludedPhrases = {u"forbidden phrase"_s};
    welcome.parentMinSpeed = 7;
    welcome.parentSpeedRatio = 30;
    welcome.queuedMessages = {fromserver::PrivateMessage{41, 1, u"alice"_s, u"while you were away"_s, false},
                              fromserver::PrivateMessage{42, 2, u"bob"_s, u"hi"_s, false}};
    server.setWelcome(welcome);
    const QSignalSpy interval(&session, &ServerSession::wishlistIntervalReceived);
    const QSignalSpy privileged(&session, &ServerSession::privilegedUsersReceived);
    const QSignalSpy phrases(&session, &ServerSession::excludedSearchPhrasesReceived);
    const QSignalSpy minSpeed(&session, &ServerSession::parentMinSpeedReceived);
    const QSignalSpy ratio(&session, &ServerSession::parentSpeedRatioReceived);
    const QSignalSpy messages(&session, &ServerSession::privateMessageReceived);
    ASSERT_TRUE(logIn(session));
    ASSERT_TRUE(waitFor([&] { return messages.size() == 2; }));
    EXPECT_EQ(interval.at(0).at(0).toUInt(), 120U);
    EXPECT_EQ(privileged.at(0).at(0).toStringList(), QStringList{u"donor"_s});
    EXPECT_EQ(phrases.at(0).at(0).toStringList(), QStringList{u"forbidden phrase"_s});
    EXPECT_EQ(minSpeed.at(0).at(0).toUInt(), 7U);
    EXPECT_EQ(ratio.at(0).at(0).toUInt(), 30U);
    EXPECT_FALSE(messages.at(0).at(0).value<fromserver::PrivateMessage>().isNew);
    // Each queued message is acknowledged so the server stops re-sending it.
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::MessageAcked>(u"selecta"_s).size() == 2; }));
    EXPECT_EQ(server.receivedOf<toserver::MessageAcked>(u"selecta"_s).at(0).id, 41U);
    EXPECT_EQ(server.receivedOf<toserver::MessageAcked>(u"selecta"_s).at(1).id, 42U);
}

TEST_F(ServerSessionTest, BrokeringAndDistributedEventsBecomeSignals)
{
    const QSignalSpy privileges(&session, &ServerSession::privilegesLeftReceived);
    ASSERT_TRUE(logIn(session));
    const QSignalSpy connectRequests(&session, &ServerSession::connectToPeerRequested);
    const QSignalSpy cantConnect(&session, &ServerSession::cantConnectToPeerReceived);
    const QSignalSpy searches(&session, &ServerSession::searchRequestReceived);
    const QSignalSpy parents(&session, &ServerSession::possibleParentsReceived);
    const QSignalSpy embedded(&session, &ServerSession::embeddedMessageReceived);
    const QSignalSpy reset(&session, &ServerSession::resetDistributedReceived);
    server.sendTo(u"selecta"_s, fromserver::ConnectToPeerRequest{u"peer"_s, ConnectionType::Peer, 0x7F000001, 2234, 9,
                                                                 false, std::nullopt, std::nullopt});
    server.sendTo(u"selecta"_s, fromserver::CantConnectToPeer{9});
    server.sendTo(u"selecta"_s, fromserver::SearchRequest{u"seeker"_s, 5, u"query"_s});
    server.sendTo(u"selecta"_s, fromserver::PossibleParents{{ParentCandidate{u"p"_s, 0x7F000001, 2240}}});
    server.sendTo(u"selecta"_s, fromserver::EmbeddedMessage{3, QByteArray("payload")});
    server.sendTo(u"selecta"_s, fromserver::ResetDistributed{});
    ASSERT_TRUE(waitFor([&] { return reset.size() == 1; }));
    EXPECT_EQ(connectRequests.at(0).at(0).value<fromserver::ConnectToPeerRequest>().token, 9U);
    EXPECT_EQ(cantConnect.at(0).at(0).toUInt(), 9U);
    EXPECT_EQ(searches.at(0).at(0).value<fromserver::SearchRequest>().query, u"query"_s);
    EXPECT_EQ(parents.at(0).at(0).value<QList<ParentCandidate>>().size(), 1);
    EXPECT_EQ(embedded.at(0).at(1).toByteArray(), QByteArray("payload"));
    // The burst's CheckPrivileges gets its answer (possibly after the messages above).
    EXPECT_TRUE(waitFor([&] { return privileges.size() == 1; }));
}

TEST_F(ServerSessionTest, SettersSendUpdatesWhileLoggedIn)
{
    ASSERT_TRUE(logIn(session));
    session.setStatus(UserStatus::Away);
    session.setStatus(UserStatus::Offline); // not a status you can set: ignored
    session.setSharedCounts(3, 4);
    session.setListenPort(5000);
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::SetWaitPort>(u"selecta"_s).size() == 2; }));
    EXPECT_EQ(server.receivedOf<toserver::SetStatus>(u"selecta"_s).constLast().status, 1);
    EXPECT_EQ(server.receivedOf<toserver::SetStatus>(u"selecta"_s).size(), 2);
    EXPECT_EQ(server.receivedOf<toserver::SharedFoldersFiles>(u"selecta"_s).constLast().files, 4U);
    EXPECT_EQ(server.receivedOf<toserver::SetWaitPort>(u"selecta"_s).constLast().port, 5000U);
}

// --- budgeted sends --------------------------------------------------------------------------------------------------

TEST_F(ServerSessionTest, BudgetedSendsNeedTheirOwnPermit)
{
    // Not logged in: nothing goes out.
    EXPECT_FALSE(session.fileSearch(1, u"q"_s, permit(Operation::Search)));
    ASSERT_TRUE(logIn(session));
    // A permit for another operation is refused.
    EXPECT_FALSE(session.fileSearch(1, u"q"_s, permit(Operation::Browse)));
    EXPECT_FALSE(session.userSearch(u"u"_s, 1, u"q"_s, permit(Operation::PrivateMessage)));
    EXPECT_FALSE(session.wishlistSearch(1, u"q"_s, permit(Operation::PeerLookup)));
    EXPECT_FALSE(session.sendPrivateMessage(u"u"_s, u"hi"_s, permit(Operation::RoomMessage)));
    EXPECT_FALSE(session.getPeerAddress(u"u"_s, permit(Operation::Search)));
    EXPECT_FALSE(session.requestPeerConnection(5, u"u"_s, ConnectionType::Peer, permit(Operation::Search)));
    EXPECT_FALSE(session.requestIndirectConnection(5, u"u"_s, ConnectionType::Peer, permit(Operation::Browse)));
    // A used permit is refused too.
    selecta::governor::Permit used = permit(Operation::Search);
    ASSERT_TRUE(used.consume(Operation::Search));
    EXPECT_FALSE(session.fileSearch(1, u"q"_s, std::move(used)));
    settle();
    EXPECT_TRUE(server.receivedOf<toserver::FileSearch>(u"selecta"_s).isEmpty());
    EXPECT_TRUE(server.receivedOf<toserver::UserSearch>(u"selecta"_s).isEmpty());
    EXPECT_TRUE(server.receivedOf<toserver::WishlistSearch>(u"selecta"_s).isEmpty());
    EXPECT_TRUE(server.receivedOf<toserver::MessageUser>(u"selecta"_s).isEmpty());
    EXPECT_TRUE(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).isEmpty());
    EXPECT_TRUE(server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).isEmpty());

    // The right permits go through.
    EXPECT_TRUE(session.fileSearch(7, u"artist"_s, permit(Operation::Search)));
    EXPECT_TRUE(session.userSearch(u"bob"_s, 8, u"album"_s, permit(Operation::Search)));
    EXPECT_TRUE(session.wishlistSearch(9, u"wish"_s, permit(Operation::Search)));
    EXPECT_TRUE(session.sendPrivateMessage(u"bob"_s, u"hello"_s, permit(Operation::PrivateMessage)));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::MessageUser>(u"selecta"_s).size() == 1; }));
    EXPECT_EQ(server.receivedOf<toserver::FileSearch>(u"selecta"_s).at(0).token, 7U);
    EXPECT_EQ(server.receivedOf<toserver::UserSearch>(u"selecta"_s).at(0).username, u"bob"_s);
    EXPECT_EQ(server.receivedOf<toserver::WishlistSearch>(u"selecta"_s).at(0).query, u"wish"_s);
}

TEST_F(ServerSessionTest, ChatLinesAreFlattenedAndBounded)
{
    QString line = u"one\r\ntwo\nthree"_s;
    EXPECT_EQ(prepareChatLine(line), ChatTextError::None);
    EXPECT_EQ(line, u"one two three"_s);
    QString empty = u"\r"_s;
    EXPECT_EQ(prepareChatLine(empty), ChatTextError::Empty);
    QString longLine(protocol::kChatLineMaxBytes + 1, u'x');
    EXPECT_EQ(prepareChatLine(longLine), ChatTextError::TooLong);
    QString exact(protocol::kChatLineMaxBytes / 2, u'é'); // two bytes each in UTF-8
    EXPECT_EQ(prepareChatLine(exact), ChatTextError::None);

    ASSERT_TRUE(logIn(session));
    EXPECT_FALSE(session.sendPrivateMessage(u"bob"_s, QString(3000, u'x'), permit(Operation::PrivateMessage)));
    EXPECT_TRUE(session.sendPrivateMessage(u"bob"_s, u"a\nb"_s, permit(Operation::PrivateMessage)));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::MessageUser>(u"selecta"_s).size() == 1; }));
    EXPECT_EQ(server.receivedOf<toserver::MessageUser>(u"selecta"_s).at(0).message, u"a b"_s);
}

TEST_F(ServerSessionTest, OneAddressLookupServesConcurrentConnectionAttempts)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy addresses(&session, &ServerSession::peerAddressReceived);
    EXPECT_TRUE(session.requestPeerConnection(11, u"peer"_s, ConnectionType::Peer, permit(Operation::PeerLookup)));
    EXPECT_TRUE(session.requestPeerConnection(12, u"peer"_s, ConnectionType::File, permit(Operation::PeerLookup)));
    EXPECT_FALSE(session.requestPeerConnection(0, u"peer"_s, ConnectionType::Peer, permit(Operation::PeerLookup)));
    ASSERT_TRUE(waitFor([&] { return addresses.size() == 1; }));
    EXPECT_EQ(server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).size(), 2);
    EXPECT_EQ(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).size(), 1);
    // Unknown user: the server answers 0.0.0.0, port 0.
    const auto address = addresses.at(0).at(0).value<fromserver::PeerAddress>();
    EXPECT_EQ(address.ip, 0U);
    EXPECT_EQ(address.port, 0U);
    // The reply closed the lookup: the next attempt asks again.
    EXPECT_TRUE(session.requestPeerConnection(13, u"peer"_s, ConnectionType::Peer, permit(Operation::PeerLookup)));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).size() == 2; }));
    // A known-address attempt sends only ConnectToPeer.
    EXPECT_TRUE(
        session.requestIndirectConnection(14, u"peer"_s, ConnectionType::Distributed, permit(Operation::PeerLookup)));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).size() == 4; }));
    EXPECT_EQ(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).size(), 2);
}

// --- rooms (D32) -----------------------------------------------------------------------------------------------------

TEST_F(ServerSessionTest, JoinSayAndLeaveARoom)
{
    ServerSession other;
    ASSERT_TRUE(logIn(session));
    ASSERT_TRUE(logIn(other, u"other"_s));
    const QSignalSpy joined(&session, &ServerSession::roomJoined);
    const QSignalSpy userJoined(&session, &ServerSession::roomUserJoined);
    const QSignalSpy userLeft(&session, &ServerSession::roomUserLeft);
    const QSignalSpy lines(&session, &ServerSession::roomMessageReceived);
    const QSignalSpy left(&session, &ServerSession::roomLeft);

    EXPECT_FALSE(session.sayInRoom(u"Lobby"_s, u"too early"_s, permit(Operation::RoomMessage))); // not joined
    ASSERT_TRUE(session.joinRoom(u"Lobby"_s));
    ASSERT_TRUE(waitFor([&] { return joined.size() == 1; }));
    // Our own UserJoinedRoom (sent before the reply) is not reported.
    EXPECT_TRUE(userJoined.isEmpty());
    EXPECT_EQ(joined.at(0).at(0).value<fromserver::RoomJoined>().members.size(), 1);
    EXPECT_EQ(session.joinedRooms(), QStringList{u"Lobby"_s});

    ASSERT_TRUE(other.joinRoom(u"Lobby"_s));
    ASSERT_TRUE(waitFor([&] { return userJoined.size() == 1; }));
    EXPECT_EQ(userJoined.at(0).at(0).value<fromserver::RoomUserJoined>().member.username, u"other"_s);

    ASSERT_TRUE(session.sayInRoom(u"Lobby"_s, u"hello room"_s, permit(Operation::RoomMessage)));
    ASSERT_TRUE(waitFor([&] { return lines.size() == 1; })); // the server echoes our own line
    EXPECT_EQ(lines.at(0).at(0).value<fromserver::RoomMessage>().username, u"selecta"_s);

    other.leaveRoom(u"Lobby"_s);
    ASSERT_TRUE(waitFor([&] { return userLeft.size() == 1; }));
    ASSERT_TRUE(session.leaveRoom(u"Lobby"_s));
    ASSERT_TRUE(waitFor([&] { return left.size() == 1; }));
    EXPECT_TRUE(session.joinedRooms().isEmpty());
    EXPECT_FALSE(session.leaveRoom(u"Lobby"_s));
}

TEST_F(ServerSessionTest, RoomNamesAreValidatedBeforeSending)
{
    EXPECT_EQ(validateRoomName(u"The Lobby"_s), ChatTextError::None);
    EXPECT_EQ(validateRoomName(u""_s), ChatTextError::InvalidRoomName);
    EXPECT_EQ(validateRoomName(u" lead"_s), ChatTextError::InvalidRoomName);
    EXPECT_EQ(validateRoomName(u"trail "_s), ChatTextError::InvalidRoomName);
    EXPECT_EQ(validateRoomName(u"two  spaces"_s), ChatTextError::InvalidRoomName);
    EXPECT_EQ(validateRoomName(u"ünicode"_s), ChatTextError::InvalidRoomName);
    EXPECT_EQ(validateRoomName(QString(25, u'r')), ChatTextError::InvalidRoomName);
    EXPECT_EQ(validateRoomName(QString(24, u'r')), ChatTextError::None);
    ASSERT_TRUE(logIn(session));
    EXPECT_FALSE(session.joinRoom(u" bad"_s));
    settle();
    EXPECT_TRUE(server.receivedOf<toserver::JoinRoom>(u"selecta"_s).isEmpty());
}

TEST_F(ServerSessionTest, UnsolicitedRoomIsLeftAndEventsForOtherRoomsAreDropped)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy joined(&session, &ServerSession::roomJoined);
    const QSignalSpy lines(&session, &ServerSession::roomMessageReceived);
    server.sendTo(u"selecta"_s, fromserver::RoomJoined{u"surprise"_s, {}, std::nullopt, {}});
    server.sendTo(u"selecta"_s, fromserver::RoomMessage{u"elsewhere"_s, u"x"_s, u"not for us"_s});
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::LeaveRoom>(u"selecta"_s).size() == 1; }));
    EXPECT_EQ(server.receivedOf<toserver::LeaveRoom>(u"selecta"_s).at(0).room, u"surprise"_s);
    settle();
    EXPECT_TRUE(joined.isEmpty());
    EXPECT_TRUE(lines.isEmpty());
}

TEST_F(ServerSessionTest, CantCreateRoomFailsTheJoin)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy failed(&session, &ServerSession::roomJoinFailed);
    ASSERT_TRUE(session.joinRoom(u"private"_s));
    server.sendTo(u"selecta"_s, fromserver::CantCreateRoom{u"private"_s});
    server.sendTo(u"selecta"_s, fromserver::CantCreateRoom{u"never asked"_s});
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    settle();
    EXPECT_EQ(failed.size(), 1);
    EXPECT_EQ(failed.at(0).at(0).toString(), u"private"_s);
}

TEST_F(ServerSessionTest, RoomsAreForgottenWhenTheSessionEnds)
{
    ASSERT_TRUE(logIn(session));
    const QSignalSpy joined(&session, &ServerSession::roomJoined);
    ASSERT_TRUE(session.joinRoom(u"Lobby"_s));
    ASSERT_TRUE(waitFor([&] { return joined.size() == 1; }));
    session.disconnectFromServer();
    EXPECT_TRUE(session.joinedRooms().isEmpty());
}
