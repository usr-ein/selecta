// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/ServerGuard.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::governor::ServerKind;
namespace guard = selecta::governor;

namespace {

struct Case
{
    const char *host;
    ServerKind kind;
};

std::ostream &operator<<(std::ostream &os, const Case &c)
{
    return os << c.host;
}

class ClassifyServerHost : public ::testing::TestWithParam<Case>
{};

} // namespace

TEST_P(ClassifyServerHost, Classifies)
{
    const Case &c = GetParam();
    EXPECT_EQ(guard::classifyServerHost(QString::fromUtf8(c.host)), c.kind) << c.host;
}

INSTANTIATE_TEST_SUITE_P(
    Hosts, ClassifyServerHost,
    ::testing::Values(
        // Local test servers
        Case{"127.0.0.1", ServerKind::Local}, Case{"127.8.9.10", ServerKind::Local}, Case{"::1", ServerKind::Local},
        Case{"10.0.0.5", ServerKind::Local}, Case{"172.16.0.1", ServerKind::Local},
        Case{"172.31.255.254", ServerKind::Local}, Case{"192.168.1.10", ServerKind::Local},
        Case{"169.254.10.10", ServerKind::Local}, Case{"fe80::1", ServerKind::Local},
        Case{"fd12:3456::1", ServerKind::Local}, Case{"::ffff:127.0.0.1", ServerKind::Local},
        Case{"localhost", ServerKind::Local}, Case{" LocalHost. ", ServerKind::Local},
        // The live network
        Case{"server.slsknet.org", ServerKind::Live}, Case{"SERVER.SLSKNET.ORG.", ServerKind::Live},
        Case{"vps.slsknet.org", ServerKind::Live}, Case{"slsknet.org", ServerKind::Live},
        Case{"8.8.8.8", ServerKind::Live}, Case{"172.32.0.1", ServerKind::Live}, Case{"100.64.0.1", ServerKind::Live},
        Case{"2001:4860:4860::8888", ServerKind::Live}, Case{"::ffff:8.8.8.8", ServerKind::Live},
        // Names that must be resolved first
        Case{"soulfind", ServerKind::Unresolved}, Case{"example.org", ServerKind::Unresolved},
        Case{"notslsknet.org", ServerKind::Unresolved},
        // Never a server
        Case{"", ServerKind::Invalid}, Case{"   ", ServerKind::Invalid}, Case{"0.0.0.0", ServerKind::Invalid},
        Case{"::", ServerKind::Invalid}, Case{"224.0.0.1", ServerKind::Invalid},
        Case{"255.255.255.255", ServerKind::Invalid}, Case{"ff02::1", ServerKind::Invalid}));

TEST(ServerGuard, LocalServersAreAlwaysAllowed)
{
    EXPECT_TRUE(guard::serverAllowed(u"127.0.0.1"_s, false));
    EXPECT_TRUE(guard::serverAllowed(u"localhost"_s, true));
    EXPECT_TRUE(guard::addressAllowed(QHostAddress(u"192.168.0.2"_s), false));
}

TEST(ServerGuard, LiveServerNeedsTheBuildOptionAndTheUser)
{
    EXPECT_FALSE(guard::serverAllowed(u"server.slsknet.org"_s, false));
    EXPECT_FALSE(guard::addressAllowed(QHostAddress(u"8.8.8.8"_s), false));
    EXPECT_EQ(guard::serverAllowed(u"server.slsknet.org"_s, true), guard::liveServerCompiledIn());
    EXPECT_EQ(guard::addressAllowed(QHostAddress(u"8.8.8.8"_s), true), guard::liveServerCompiledIn());
}

TEST(ServerGuard, TestBuildsNeverReachTheLiveServer)
{
    // The test suite must be built without SELECTA_ENABLE_LIVE_SERVER (CLAUDE.md network safety rules).
    EXPECT_FALSE(guard::liveServerCompiledIn());
}

TEST(ServerGuard, UnresolvedAndInvalidAreRefused)
{
    EXPECT_FALSE(guard::serverAllowed(u"soulfind"_s, true));
    EXPECT_FALSE(guard::serverAllowed(u"0.0.0.0"_s, true));
    EXPECT_FALSE(guard::addressAllowed(QHostAddress(), true));
}
