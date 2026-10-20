// SPDX-License-Identifier: GPL-3.0-or-later
// PRD NET-3 in Settings → Soulseek: the switch for opening the port on the router, the line saying what happened, and
// the reachability line.
#include "app/views/SettingsView.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::SettingsView;
using selecta::core::NetworkConfig;
using selecta::core::NetworkStatus;
using selecta::core::PortMappingStatus;
using State = PortMappingStatus::State;

namespace {

class PortMappingSettingsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NetworkConfig config;
        config.listenPort = 2234;
        view = std::make_unique<SettingsView>(selecta::app::BuildInfo{}, selecta::app::Appearance::System,
                                              selecta::core::LibraryConfig{}, config);
    }

    QString text(const QString &objectName) const
    {
        const auto *label = view->findChild<QLabel *>(objectName);
        return label != nullptr ? label->text() : u"(missing)"_s;
    }

    QString stateLine(const PortMappingStatus &mapping, int peers = 0)
    {
        NetworkStatus status;
        status.portMapping = mapping;
        status.peersReachedUs = peers;
        view->setNetworkStatus(status);
        return text(u"settings.network.portMappingState"_s);
    }

    std::unique_ptr<SettingsView> view;
};

PortMappingStatus mapping(State state)
{
    PortMappingStatus status;
    status.state = state;
    status.internalPort = 2234;
    return status;
}

} // namespace

TEST_F(PortMappingSettingsTest, TheSwitchIsOnByDefaultAndSaved)
{
    auto *box = view->findChild<QCheckBox *>(u"settings.network.portMapping"_s);
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(box->text(), u"Open the port on the router automatically (UPnP/NAT-PMP)"_s);
    EXPECT_TRUE(box->isChecked());

    const QSignalSpy changed(view.get(), &SettingsView::networkConfigChanged);
    box->setChecked(false);
    view->findChild<QPushButton *>(u"settings.network.save"_s)->click();
    ASSERT_EQ(changed.size(), 1);
    EXPECT_FALSE(changed.at(0).at(0).value<NetworkConfig>().portMapping);
}

TEST_F(PortMappingSettingsTest, FailuresSayWhichPortToForward)
{
    const QString forward = u"Forward TCP port 2234 to this Mac on your router."_s;
    EXPECT_TRUE(stateLine(mapping(State::Off)).contains(forward));
    PortMappingStatus noRouter = mapping(State::NoRouter);
    noRouter.givenUp = true;
    EXPECT_EQ(stateLine(noRouter), u"No router answered. "_s + forward);
    PortMappingStatus refused = mapping(State::Refused);
    refused.reason = u"The router refused (UPnP error 606: Action not authorized)."_s;
    EXPECT_EQ(stateLine(refused), refused.reason + u' ' + forward);
    EXPECT_TRUE(stateLine(mapping(State::Unavailable)).contains(u"--port-mapping"_s));
}

TEST_F(PortMappingSettingsTest, AMappedPortSaysWhereIt)
{
    PortMappingStatus mapped = mapping(State::Mapped);
    mapped.protocol = selecta::core::portmap::Protocol::Upnp;
    mapped.externalPort = 2234;
    mapped.externalAddress = u"203.0.113.9"_s;
    EXPECT_EQ(stateLine(mapped), u"Port 2234 is open on the router (UPnP): peers reach you at 203.0.113.9:2234."_s);

    mapped.behindAnotherNat = true;
    EXPECT_TRUE(stateLine(mapped).contains(u"may still not reach you"_s));

    mapped.behindAnotherNat = false;
    mapped.externalAddress.clear();
    mapped.externalPort = 50123;
    mapped.protocol = selecta::core::portmap::Protocol::NatPmp;
    EXPECT_EQ(stateLine(mapped), u"Port 2234 is open on the router as port 50123 (NAT-PMP)."_s);
}

TEST_F(PortMappingSettingsTest, ReachabilityCountsPeersThatConnectedDirectly)
{
    stateLine(mapping(State::Mapped), 0);
    EXPECT_EQ(text(u"settings.network.reachability"_s), u"Reachable: not confirmed yet."_s);
    stateLine(mapping(State::Mapped), 1);
    EXPECT_EQ(text(u"settings.network.reachability"_s), u"Reachable: 1 peer connected directly."_s);
    stateLine(mapping(State::NoRouter), 7);
    EXPECT_EQ(text(u"settings.network.reachability"_s), u"Reachable: 7 peers connected directly."_s);
}
