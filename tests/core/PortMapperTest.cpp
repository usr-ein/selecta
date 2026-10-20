// SPDX-License-Identifier: GPL-3.0-or-later
// PRD NET-3: the listening port opened on the router (NAT-PMP, then UPnP), renewed, moved and removed; against loopback
// fakes only.
#include "core/PortMapper.h"

#include "core/FakeRouter.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QUdpSocket>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::PortMapper;
using selecta::core::PortMappingStatus;
using selecta::core::portmap::Endpoint;
using selecta::core::portmap::Protocol;
using selecta::core::testing::FakeIgd;
using selecta::core::testing::FakeNatPmp;
using State = PortMappingStatus::State;

namespace {

constexpr quint16 kPort = 42234; // only a number in the requests: nothing listens on it
constexpr std::chrono::milliseconds kQuickNatPmpWait = 50ms; // the first NAT-PMP wait where nobody answers

class PortMapperTest : public ::testing::Test
{
protected:
    // Options aimed at the fakes, with short waits. `natPmp` / `igd`: false leaves that protocol with nobody answering.
    //
    // The fakes answer from this thread's event loop, which a loaded runner can hold up for a while. So when the
    // NAT-PMP fake answers, the mapper waits for it up to 1 + 2 s: a slow answer is never taken for a router that went
    // away, which would add an attempt. A port where nobody answers costs only 50 + 100 ms; tests that silence the fake
    // set that short wait themselves.
    PortMapper::Options options(bool natPmp, bool igd)
    {
        PortMapper::Options o;
        const quint16 natPmpPort = natPmp ? natPmpFake.port() : silent.localPort();
        o.gateway = [] { return std::optional<QHostAddress>(QHostAddress(QHostAddress::LocalHost)); };
        o.natPmpPort = natPmpPort;
        o.ssdp = Endpoint{QHostAddress(QHostAddress::LocalHost), igd ? igdFake.ssdpPort() : silent.localPort()};
        o.bindAddress = QHostAddress(QHostAddress::LocalHost);
        o.timing = {natPmp ? 1000ms : kQuickNatPmpWait, 2, 300ms, 1000ms};
        o.quitTiming = {50ms, 1, 0ms, 300ms};
        o.minRenewal = 200ms;
        o.retryDelays = {100ms, 100ms};
        return o;
    }

    void SetUp() override { ASSERT_TRUE(silent.bind(QHostAddress::LocalHost, 0)); }

    static bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds timeout = 5s)
    {
        return QTest::qWaitFor(condition, QDeadlineTimer(timeout));
    }

    static bool reaches(PortMapper &mapper, State state)
    {
        return waitFor([&] { return mapper.status().state == state; });
    }

    FakeNatPmp natPmpFake;
    FakeIgd igdFake;
    QUdpSocket silent; // a port where nothing ever answers
};

} // namespace

// --- NAT-PMP ---------------------------------------------------------------------------------------------------------

TEST_F(PortMapperTest, MapsTheListeningPortWithNatPmp)
{
    PortMapper mapper(options(true, false));
    EXPECT_EQ(mapper.status().state, State::Off);
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    const PortMappingStatus status = mapper.status();
    EXPECT_EQ(status.protocol, Protocol::NatPmp);
    EXPECT_EQ(status.internalPort, kPort);
    EXPECT_EQ(status.externalPort, kPort); // the same port, since it was free
    EXPECT_EQ(status.externalAddress, u"203.0.113.7"_s);
    EXPECT_FALSE(status.behindAnotherNat);
    EXPECT_EQ(natPmpFake.mappings.value(kPort), kPort);
    ASSERT_FALSE(natPmpFake.requests.isEmpty());
    EXPECT_EQ(natPmpFake.requests.last().lifetime, 7200U);
    EXPECT_EQ(igdFake.searches, 0); // NAT-PMP answered: no UPnP search
}

TEST_F(PortMapperTest, TakesTheExternalPortTheRouterGives)
{
    natPmpFake.takenExternalPorts.insert(kPort, 1);
    PortMapper mapper(options(true, false));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    EXPECT_EQ(mapper.status().externalPort, kPort + 1);
}

TEST_F(PortMapperTest, RenewsBeforeTheLeaseEnds)
{
    natPmpFake.grantedLifetime = 1; // renewed at half: 500 ms
    PortMapper mapper(options(true, false));
    const QSignalSpy changes(&mapper, &PortMapper::statusChanged);
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    const qsizetype afterMapping = changes.size();
    ASSERT_TRUE(waitFor([&] { return natPmpFake.mapRequests() >= 3; }));
    EXPECT_EQ(mapper.status().state, State::Mapped);
    EXPECT_EQ(changes.size(), afterMapping); // a renewal that changes nothing says nothing
    for (const FakeNatPmp::Request &request : std::as_const(natPmpFake.requests)) {
        if (request.opcode == 2) {
            EXPECT_EQ(request.externalPort, kPort); // asks for the same external port each time
        }
    }
}

TEST_F(PortMapperTest, NeverRenewsFasterThanTheFloor)
{
    natPmpFake.grantedLifetime = 1; // half of it is 500 ms, well under the floor
    PortMapper::Options o = options(true, false);
    o.minRenewal = 1500ms;
    PortMapper mapper(o);
    QElapsedTimer sinceStart;
    sinceStart.start();
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    ASSERT_TRUE(waitFor([&] { return natPmpFake.mapRequests() >= 2; }));
    // The renewal timer starts once the mapping is made, after this clock: a slow runner only makes the renewal look
    // later, never earlier. Qt's coarse timers may fire up to 5 % early.
    EXPECT_GE(sinceStart.elapsed(), 1500 * 95 / 100);
}

TEST_F(PortMapperTest, ARetransmittedRequestIsNotANewOne)
{
    natPmpFake.loseFirstAnswers = true; // the router acts on each request; its first answer never comes back
    PortMapper::Options o = options(true, false);
    o.timing.natPmpFirstWait = 200ms; // asked again after 200 ms, then waits 400 + 800 ms
    o.timing.natPmpTries = 3;
    const auto copies = [&](bool removal) {
        int n = 0;
        for (const FakeNatPmp::Request &request : std::as_const(natPmpFake.requests)) {
            n += request.opcode == 2 && request.retransmission && (request.lifetime == 0) == removal ? 1 : 0;
        }
        return n;
    };
    PortMapper mapper(o);
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    EXPECT_GE(copies(false), 1); // mapped from the copy's answer
    EXPECT_EQ(natPmpFake.mapRequests(), 1);
    mapper.configure(false, kPort);
    ASSERT_TRUE(waitFor([&] { return copies(true) >= 1; }));
    EXPECT_EQ(natPmpFake.removals(), 1);
    EXPECT_TRUE(natPmpFake.mappings.isEmpty());
}

TEST_F(PortMapperTest, RemovesTheMappingAtQuit)
{
    auto mapper = std::make_unique<PortMapper>(options(true, false));
    mapper->configure(true, kPort);
    ASSERT_TRUE(reaches(*mapper, State::Mapped));
    mapper.reset(); // the fake answers only once the event loop runs again: the removal request is what counts
    ASSERT_TRUE(waitFor([&] { return natPmpFake.removals() == 1; }));
    EXPECT_FALSE(natPmpFake.mappings.contains(kPort));
}

TEST_F(PortMapperTest, TurningItOffRemovesTheMapping)
{
    PortMapper mapper(options(true, false));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    mapper.configure(false, kPort);
    EXPECT_EQ(mapper.status().state, State::Off);
    ASSERT_TRUE(waitFor([&] { return natPmpFake.removals() >= 1; }));
    EXPECT_EQ(natPmpFake.removals(), 1);
    EXPECT_TRUE(natPmpFake.mappings.isEmpty());
    EXPECT_FALSE(mapper.mapping().has_value());
}

TEST_F(PortMapperTest, NotListeningMeansNothingIsMapped)
{
    PortMapper mapper(options(true, false));
    mapper.configure(true, 0);
    EXPECT_EQ(mapper.status().state, State::Idle);
    QTest::qWait(200);
    EXPECT_TRUE(natPmpFake.requests.isEmpty());
}

TEST_F(PortMapperTest, APortChangeMovesTheMapping)
{
    PortMapper mapper(options(true, false));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    mapper.configure(true, kPort + 10);
    ASSERT_TRUE(
        waitFor([&] { return mapper.status().state == State::Mapped && mapper.status().internalPort == kPort + 10; }));
    ASSERT_TRUE(waitFor([&] { return !natPmpFake.mappings.contains(kPort); }));
    EXPECT_EQ(natPmpFake.mappings.value(kPort + 10), kPort + 10);
    EXPECT_EQ(natPmpFake.removals(), 1);
}

TEST_F(PortMapperTest, APortChangeDuringAnAttemptMapsOnlyTheNewPort)
{
    PortMapper mapper(options(true, false));
    mapper.configure(true, kPort);
    mapper.configure(true, kPort + 10); // at once, while the first attempt is on the worker
    ASSERT_TRUE(
        waitFor([&] { return mapper.status().state == State::Mapped && mapper.status().internalPort == kPort + 10; }));
    QTest::qWait(200);
    EXPECT_FALSE(natPmpFake.mappings.contains(kPort));
    EXPECT_TRUE(natPmpFake.mappings.contains(kPort + 10));
}

TEST_F(PortMapperTest, ARouterBehindAnotherNatIsFlagged)
{
    natPmpFake.external = QHostAddress(u"100.64.12.34"_s); // carrier-grade NAT
    PortMapper mapper(options(true, false));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    EXPECT_TRUE(mapper.status().behindAnotherNat);
}

TEST_F(PortMapperTest, ARefusalIsReportedWithItsReason)
{
    natPmpFake.mapResult = 2; // not authorised
    PortMapper mapper(options(true, false));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Refused));
    EXPECT_TRUE(mapper.status().reason.contains(u"NAT-PMP"_s)) << mapper.status().reason.toStdString();
    EXPECT_TRUE(natPmpFake.mappings.isEmpty());
}

// --- no router, retries ----------------------------------------------------------------------------------------------

TEST_F(PortMapperTest, NoRouterGivesUpAfterAFewAttempts)
{
    natPmpFake.silent = true;
    PortMapper::Options o = options(true, false);
    o.timing.natPmpFirstWait = kQuickNatPmpWait;
    PortMapper mapper(o);
    mapper.configure(true, kPort);
    ASSERT_TRUE(waitFor([&] { return mapper.status().state == State::NoRouter && mapper.status().givenUp; }, 10s));
    const int askedSoFar = natPmpFake.received;
    EXPECT_EQ(askedSoFar, 3 * 2); // three attempts of two tries each
    QTest::qWait(500);            // longer than every retry delay
    EXPECT_EQ(mapper.status().state, State::NoRouter);
    EXPECT_EQ(natPmpFake.received, askedSoFar);
    // A new port starts over.
    natPmpFake.silent = false;
    mapper.configure(true, kPort + 1);
    EXPECT_TRUE(reaches(mapper, State::Mapped));
}

TEST_F(PortMapperTest, NoGatewayAndNoIgdIsNoRouter)
{
    PortMapper::Options o = options(false, false);
    o.gateway = [] { return std::optional<QHostAddress>(); };
    o.retryDelays = {};
    PortMapper mapper(o);
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::NoRouter));
    EXPECT_TRUE(mapper.status().givenUp);
    EXPECT_TRUE(mapper.status().reason.isEmpty());
}

TEST_F(PortMapperTest, AFailedRenewalCountsAsAFailedAttempt)
{
    natPmpFake.grantedLifetime = 1;
    PortMapper::Options o = options(true, false);
    o.timing.natPmpFirstWait = kQuickNatPmpWait; // the router goes silent
    PortMapper mapper(o);
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    natPmpFake.silent = true; // the router went away
    ASSERT_TRUE(reaches(mapper, State::NoRouter));
    EXPECT_FALSE(mapper.mapping().has_value());
    natPmpFake.silent = false; // and came back: the retry maps again
    EXPECT_TRUE(reaches(mapper, State::Mapped));
}

// --- UPnP ------------------------------------------------------------------------------------------------------------

TEST_F(PortMapperTest, FallsBackToUpnp)
{
    PortMapper mapper(options(false, true));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    const PortMappingStatus status = mapper.status();
    EXPECT_EQ(status.protocol, Protocol::Upnp);
    EXPECT_EQ(status.externalPort, kPort);
    EXPECT_EQ(status.externalAddress, u"203.0.113.9"_s);
    const auto adds = igdFake.callsOf(u"AddPortMapping"_s);
    ASSERT_EQ(adds.size(), 1);
    EXPECT_EQ(adds.at(0).arguments.value(u"NewInternalClient"_s), u"127.0.0.1"_s);
    EXPECT_EQ(adds.at(0).arguments.value(u"NewInternalPort"_s), QString::number(kPort));
    EXPECT_EQ(adds.at(0).arguments.value(u"NewProtocol"_s), u"TCP"_s);
    EXPECT_EQ(adds.at(0).arguments.value(u"NewLeaseDuration"_s), u"3600"_s);
    EXPECT_EQ(adds.at(0).arguments.value(u"NewPortMappingDescription"_s), u"Selecta"_s);
}

TEST_F(PortMapperTest, ReadsAChunkedDescription)
{
    igdFake.chunked = true;
    PortMapper mapper(options(false, true));
    mapper.configure(true, kPort);
    EXPECT_TRUE(reaches(mapper, State::Mapped));
}

TEST_F(PortMapperTest, UpnpRemovesTheMappingAtQuit)
{
    auto mapper = std::make_unique<PortMapper>(options(false, true));
    mapper->configure(true, kPort);
    ASSERT_TRUE(reaches(*mapper, State::Mapped));
    mapper.reset();
    ASSERT_TRUE(waitFor([&] { return !igdFake.callsOf(u"DeletePortMapping"_s).isEmpty(); }));
    EXPECT_TRUE(igdFake.mappings.isEmpty());
}

TEST_F(PortMapperTest, UpnpRefusalIsReported)
{
    igdFake.addPortMappingError = 606;
    PortMapper mapper(options(false, true));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Refused));
    EXPECT_TRUE(mapper.status().reason.contains(u"606"_s)) << mapper.status().reason.toStdString();
}

TEST_F(PortMapperTest, APortTakenByAnotherDeviceGetsAnotherExternalPort)
{
    igdFake.otherDevices.insert(kPort, u"192.168.1.99"_s);
    PortMapper mapper(options(false, true));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    const quint16 external = mapper.status().externalPort;
    EXPECT_NE(external, kPort);
    EXPECT_GE(external, 49152);
    EXPECT_EQ(igdFake.mappings.value(external).second, kPort);
}

TEST_F(PortMapperTest, OurOwnLeftoverMappingIsTakenBack)
{
    igdFake.otherDevices.insert(kPort, u"127.0.0.1"_s); // left by an earlier run on this machine
    PortMapper mapper(options(false, true));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    EXPECT_EQ(mapper.status().externalPort, kPort);
}

TEST_F(PortMapperTest, PermanentLeasesOnlyAreAccepted)
{
    igdFake.permanentLeasesOnly = true;
    PortMapper mapper(options(false, true));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    ASSERT_TRUE(mapper.mapping().has_value());
    EXPECT_EQ(mapper.mapping()->lifetime.count(), 0);
}

TEST_F(PortMapperTest, ALocationOnAnotherHostIsIgnored)
{
    igdFake.locationHost = u"127.0.0.2"_s; // not the address the answer came from
    PortMapper::Options o = options(false, true);
    o.retryDelays = {};
    PortMapper mapper(o);
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::NoRouter));
    EXPECT_TRUE(igdFake.calls.isEmpty());
}

TEST_F(PortMapperTest, NatPmpRefusalStillTriesUpnp)
{
    natPmpFake.mapResult = 2;
    PortMapper mapper(options(true, true));
    mapper.configure(true, kPort);
    ASSERT_TRUE(reaches(mapper, State::Mapped));
    EXPECT_EQ(mapper.status().protocol, Protocol::Upnp);
}
