// SPDX-License-Identifier: GPL-3.0-or-later
// PRD NET-3: the wire formats of NAT-PMP, SSDP, the IGD description, SOAP and HTTP, and how untrusted answers are
// bounded and refused.
#include "core/PortMapClient.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::core::portmap;
using namespace selecta::core::portmap::detail;
using ParseState = HttpParse::State;

namespace {

const QUrl kLocation(u"http://192.168.1.1:5000/rootDesc.xml"_s);

QByteArray description(const QString &services, const QString &urlBase = {})
{
    return (u"<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\">"_s +
            (urlBase.isEmpty() ? QString() : u"<URLBase>%1</URLBase>"_s.arg(urlBase)) +
            u"<device><deviceList><device><serviceList>%1</serviceList></device></deviceList></device></root>"_s.arg(
                services))
        .toUtf8();
}

QString service(const QString &type, const QString &control)
{
    return u"<service><serviceType>urn:schemas-upnp-org:service:%1</serviceType><controlURL>%2</controlURL></service>"_s
        .arg(type, control);
}

} // namespace

// --- NAT-PMP ---------------------------------------------------------------------------------------------------------

TEST(PortMapClient, NatPmpRequestsAreRfc6886)
{
    EXPECT_EQ(natPmpAddressRequest().toHex(), "0000"_ba);
    EXPECT_EQ(natPmpMapRequest(2234, 2234, 7200).toHex(' '), "00 02 00 00 08 ba 08 ba 00 00 1c 20"_ba);
    EXPECT_EQ(natPmpMapRequest(2234, 0, 0).toHex(' '), "00 02 00 00 08 ba 00 00 00 00 00 00"_ba);
}

TEST(PortMapClient, NatPmpAnswersAreParsed)
{
    const auto address = parseNatPmpAnswer(QByteArray::fromHex("0080 0000 000003e8 cb007107"));
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->opcode, 128);
    EXPECT_EQ(address->result, 0);
    EXPECT_EQ(address->address, QHostAddress(u"203.0.113.7"_s));

    const auto mapping = parseNatPmpAnswer(QByteArray::fromHex("0082 0000 000003e8 08ba 08bb 00000e10"));
    ASSERT_TRUE(mapping.has_value());
    EXPECT_EQ(mapping->opcode, 130);
    EXPECT_EQ(mapping->internalPort, 2234);
    EXPECT_EQ(mapping->externalPort, 2235);
    EXPECT_EQ(mapping->lifetime, 3600u);
}

TEST(PortMapClient, MalformedNatPmpAnswersAreRefused)
{
    EXPECT_FALSE(parseNatPmpAnswer({}).has_value());
    EXPECT_FALSE(parseNatPmpAnswer(QByteArray::fromHex("0082 0000 000003e8")).has_value()); // short
    EXPECT_FALSE(parseNatPmpAnswer(QByteArray::fromHex("0180 0000 000003e8 cb007107")).has_value()); // version 1
    EXPECT_FALSE(parseNatPmpAnswer(QByteArray::fromHex("0002 0000 000003e8 08ba 08bb 00000e10")).has_value()); // a request
    EXPECT_FALSE(parseNatPmpAnswer(QByteArray::fromHex("0081 0000 000003e8 08ba 08bb 00000e10")).has_value()); // UDP
}

TEST(PortMapClient, NatPmpResultsAreWords)
{
    EXPECT_TRUE(natPmpResultText(0).isEmpty());
    for (quint16 result = 1; result <= 6; ++result) {
        EXPECT_FALSE(natPmpResultText(result).isEmpty());
    }
}

// --- SSDP ------------------------------------------------------------------------------------------------------------

TEST(PortMapClient, SsdpSearchNamesItsTarget)
{
    const QByteArray search =
        ssdpSearch(u"urn:schemas-upnp-org:device:InternetGatewayDevice:1"_s, ssdpMulticast());
    EXPECT_TRUE(search.startsWith("M-SEARCH * HTTP/1.1\r\n"));
    EXPECT_TRUE(search.contains("HOST: 239.255.255.250:1900\r\n"));
    EXPECT_TRUE(search.contains("MAN: \"ssdp:discover\"\r\n"));
    EXPECT_TRUE(search.contains("ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"));
    EXPECT_TRUE(search.endsWith("\r\n\r\n"));
}

TEST(PortMapClient, SsdpLocationMustBeTheSendersOwnAddress)
{
    const QHostAddress router(u"192.168.1.1"_s);
    const QByteArray answer = "HTTP/1.1 200 OK\r\nST: x\r\nlocation:  http://192.168.1.1:5000/rootDesc.xml \r\n\r\n"_ba;
    const auto location = ssdpLocation(answer, router);
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(*location, kLocation);
    EXPECT_TRUE(ssdpLocation(answer, QHostAddress(u"::ffff:192.168.1.1"_s)).has_value()); // IPv4-mapped sender

    EXPECT_FALSE(ssdpLocation(answer, QHostAddress(u"192.168.1.66"_s)).has_value()); // someone else's URL
    EXPECT_FALSE(ssdpLocation("HTTP/1.1 200 OK\r\nLOCATION: http://router.lan:5000/d.xml\r\n\r\n"_ba, router)
                     .has_value()); // never looked up by name
    EXPECT_FALSE(ssdpLocation("HTTP/1.1 200 OK\r\nLOCATION: https://192.168.1.1:5000/d.xml\r\n\r\n"_ba, router)
                     .has_value());
    EXPECT_FALSE(ssdpLocation("HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.1/d.xml\r\n\r\n"_ba, router)
                     .has_value()); // no port
    EXPECT_FALSE(ssdpLocation("NOTIFY * HTTP/1.1\r\nLOCATION: http://192.168.1.1:5000/d.xml\r\n\r\n"_ba, router)
                     .has_value());
    EXPECT_FALSE(ssdpLocation("HTTP/1.1 200 OK\r\nST: x\r\n\r\n"_ba, router).has_value());
}

// --- the device description ------------------------------------------------------------------------------------------

TEST(PortMapClient, DescriptionPrefersIpConnections)
{
    const QList<IgdService> found =
        parseDescription(description(service(u"WANPPPConnection:1"_s, u"/ctl/PPP"_s) +
                                     service(u"Layer3Forwarding:1"_s, u"/ctl/L3F"_s) +
                                     service(u"WANIPConnection:1"_s, u"/ctl/IP"_s)),
                         kLocation);
    ASSERT_EQ(found.size(), 2);
    EXPECT_EQ(found.at(0).serviceType, u"urn:schemas-upnp-org:service:WANIPConnection:1"_s);
    EXPECT_EQ(found.at(0).controlUrl, QUrl(u"http://192.168.1.1:5000/ctl/IP"_s));
    EXPECT_EQ(found.at(1).serviceType, u"urn:schemas-upnp-org:service:WANPPPConnection:1"_s);
}

TEST(PortMapClient, DescriptionControlUrlsStayOnTheDevice)
{
    // Absolute control URLs elsewhere, and a URLBase elsewhere, are ignored.
    EXPECT_TRUE(parseDescription(description(service(u"WANIPConnection:1"_s, u"http://10.0.0.9:80/ctl"_s)), kLocation)
                    .isEmpty());
    const QList<IgdService> based = parseDescription(
        description(service(u"WANIPConnection:1"_s, u"ctl/IP"_s), u"http://10.0.0.9:80/"_s), kLocation);
    ASSERT_EQ(based.size(), 1);
    EXPECT_EQ(based.at(0).controlUrl, QUrl(u"http://192.168.1.1:5000/ctl/IP"_s));
    // A URLBase on the same host, another port, is followed.
    const QList<IgdService> samehost = parseDescription(
        description(service(u"WANIPConnection:2"_s, u"/ctl/IP"_s), u"http://192.168.1.1:49000/"_s), kLocation);
    ASSERT_EQ(samehost.size(), 1);
    EXPECT_EQ(samehost.at(0).controlUrl, QUrl(u"http://192.168.1.1:49000/ctl/IP"_s));
}

TEST(PortMapClient, BrokenDescriptionsFindNothing)
{
    EXPECT_TRUE(parseDescription({}, kLocation).isEmpty());
    EXPECT_TRUE(parseDescription("<root><device>"_ba, kLocation).isEmpty());
    EXPECT_TRUE(parseDescription(description(service(u"WANIPConnection:1"_s, QString())), kLocation).isEmpty());
}

// --- SOAP ------------------------------------------------------------------------------------------------------------

TEST(PortMapClient, SoapEnvelopeEscapesItsArguments)
{
    const QByteArray envelope = soapEnvelope(u"urn:schemas-upnp-org:service:WANIPConnection:1"_s, u"AddPortMapping"_s,
                                             {{u"NewExternalPort"_s, u"2234"_s},
                                              {u"NewPortMappingDescription"_s, u"<Selecta & co>"_s}});
    EXPECT_TRUE(envelope.contains("<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"));
    EXPECT_TRUE(envelope.contains("<NewExternalPort>2234</NewExternalPort>"));
    EXPECT_TRUE(envelope.contains("<NewPortMappingDescription>&lt;Selecta &amp; co&gt;</NewPortMappingDescription>"));
    EXPECT_TRUE(envelope.contains("</u:AddPortMapping></s:Body></s:Envelope>"));
}

TEST(PortMapClient, SoapAnswersAndFaults)
{
    const SoapAnswer answer = parseSoapAnswer(
        200, "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:GetExternalIPAddressResponse "
             "xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"><NewExternalIPAddress> 203.0.113.9 "
             "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>"_ba);
    EXPECT_TRUE(answer.ok);
    EXPECT_EQ(answer.values.value(u"NewExternalIPAddress"_s), u"203.0.113.9"_s);

    const SoapAnswer fault = parseSoapAnswer(
        500, "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault><faultcode>s:Client"
             "</faultcode><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>718</errorCode>"
             "<errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault></s:Body>"
             "</s:Envelope>"_ba);
    EXPECT_FALSE(fault.ok);
    EXPECT_EQ(fault.errorCode, 718);
    EXPECT_EQ(fault.errorDescription, u"ConflictInMappingEntry"_s);

    EXPECT_FALSE(parseSoapAnswer(200, "<s:Envelope><s:Body"_ba).ok); // truncated
    EXPECT_FALSE(parseSoapAnswer(404, {}).ok);
}

// --- HTTP ------------------------------------------------------------------------------------------------------------

TEST(PortMapClient, HttpWithContentLength)
{
    const QByteArray head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"_ba;
    EXPECT_EQ(parseHttpResponse(head + "abc", false).state, ParseState::Incomplete);
    EXPECT_EQ(parseHttpResponse(head + "abc", true).state, ParseState::Malformed);
    const HttpParse done = parseHttpResponse(head + "abcdeXX", false);
    ASSERT_EQ(done.state, ParseState::Complete);
    EXPECT_EQ(done.response.status, 200);
    EXPECT_EQ(done.response.body, "abcde"_ba);
}

TEST(PortMapClient, HttpChunked)
{
    const QByteArray head = "HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked\r\n\r\n"_ba;
    EXPECT_EQ(parseHttpResponse(head + "3\r\nabc\r\n", false).state, ParseState::Incomplete);
    const HttpParse done = parseHttpResponse(head + "3;ext=1\r\nabc\r\nA\r\n0123456789\r\n0\r\n\r\n", false);
    ASSERT_EQ(done.state, ParseState::Complete);
    EXPECT_EQ(done.response.body, "abc0123456789"_ba);
    EXPECT_EQ(parseHttpResponse(head + "zz\r\nabc\r\n", false).state, ParseState::Malformed);
    EXPECT_EQ(parseHttpResponse(head + "7fffffff\r\nabc", false).state, ParseState::Malformed); // over the cap
}

TEST(PortMapClient, HttpToTheClose)
{
    const QByteArray raw = "HTTP/1.0 500 Internal Server Error\r\n\r\n<fault/>"_ba;
    EXPECT_EQ(parseHttpResponse(raw, false).state, ParseState::Incomplete);
    const HttpParse done = parseHttpResponse(raw, true);
    ASSERT_EQ(done.state, ParseState::Complete);
    EXPECT_EQ(done.response.status, 500);
    EXPECT_EQ(done.response.body, "<fault/>"_ba);
}

TEST(PortMapClient, HttpOutOfBoundsIsMalformed)
{
    EXPECT_EQ(parseHttpResponse("SSH-2.0-OpenSSH\r\n\r\n"_ba, false).state, ParseState::Malformed);
    EXPECT_EQ(parseHttpResponse("HTTP/1.1 abc OK\r\n\r\n"_ba, false).state, ParseState::Malformed);
    EXPECT_EQ(parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 99999999\r\n\r\n"_ba, false).state,
              ParseState::Malformed);
    EXPECT_EQ(parseHttpResponse("HTTP/1.1 200 OK\r\nX: "_ba + QByteArray(kHttpHeaderMax, 'a'), false).state,
              ParseState::Malformed); // headers that never end
    EXPECT_EQ(parseHttpResponse("HTTP/1.1 200 OK\r\n"_ba, true).state, ParseState::Malformed);
}

// --- addresses -------------------------------------------------------------------------------------------------------

TEST(PortMapClient, UnroutableAddresses)
{
    for (const QString &address : {u"10.1.2.3"_s, u"172.16.0.1"_s, u"172.31.255.255"_s, u"192.168.0.1"_s,
                                   u"100.64.0.1"_s, u"100.127.255.254"_s, u"169.254.1.1"_s, u"127.0.0.1"_s,
                                   u"0.0.0.0"_s}) {
        EXPECT_TRUE(isUnroutable(QHostAddress(address))) << address.toStdString();
    }
    for (const QString &address : {u"203.0.113.7"_s, u"172.32.0.1"_s, u"100.128.0.1"_s, u"8.8.8.8"_s}) {
        EXPECT_FALSE(isUnroutable(QHostAddress(address))) << address.toStdString();
    }
}
