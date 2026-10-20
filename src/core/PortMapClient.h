// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <optional>
#include <utility>

namespace selecta::core::portmap {

// The router's port-mapping protocols (PRD NET-3), as blocking calls for PortMapper's worker thread: NAT-PMP (RFC 6886)
// to the default gateway, then UPnP IGD (SSDP discovery, the device description, SOAP control). Written here rather
// than taken from miniupnpc so that every answer is bounded and every wait has a deadline: miniupnpc's HTTP reader
// grows its buffer to whatever Content-Length a device claims and has no overall time limit.
//
// Everything a router or any other device on the network sends is untrusted: datagrams and HTTP answers are size-capped,
// NAT-PMP answers count only from the gateway's address and port, an SSDP answer only when its LOCATION is an IP literal
// equal to the sender's address, and a control URL only on that same host. Nothing is ever looked up by name.
//
// Threading: any thread, no event loop needed (Qt sockets in blocking mode). Each call creates and destroys its own
// sockets. Every wait is sliced so that `cancel` stops a call within about 100 ms.

using Cancel = std::atomic<bool>;

struct Endpoint
{
    QHostAddress address;
    quint16 port = 0;

    bool operator==(const Endpoint &) const = default;
};

// UPnP's multicast address and port (239.255.255.250:1900).
Endpoint ssdpMulticast();
inline constexpr quint16 kNatPmpPort = 5351;

enum class Protocol : quint8 {
    NatPmp,
    Upnp,
};

// A mapping on the router: what renewing or removing it needs.
struct Mapping
{
    Protocol protocol = Protocol::NatPmp;
    quint16 internalPort = 0;
    quint16 externalPort = 0;
    QHostAddress externalAddress;     // the router's public address; null when it didn't say
    std::chrono::seconds lifetime{0}; // as granted; 0: until removed (a permanent UPnP lease)
    Endpoint gateway;                 // NAT-PMP: where requests go
    QUrl controlUrl;                  // UPnP: the WAN connection service
    QString serviceType;
    QHostAddress internalClient; // UPnP: our address on the router's network, where it forwards to

    bool operator==(const Mapping &) const = default;
};

struct Timing
{
    std::chrono::milliseconds natPmpFirstWait{250}; // RFC 6886 §3.1: 250 ms, doubled at each try
    int natPmpTries = 3;                            // 250 + 500 + 1000 ms
    std::chrono::milliseconds ssdpWait{2000};       // how long to listen for SSDP answers
    std::chrono::milliseconds httpTimeout{3000};    // one HTTP exchange, from connecting to the last byte
};

struct MapRequest
{
    quint16 internalPort = 0;
    quint16 externalPort = 0;              // wanted; 0: the same as the internal port
    std::optional<Endpoint> natPmpGateway; // nullopt: NAT-PMP is skipped (no default gateway)
    std::optional<Endpoint> ssdp;          // nullopt: UPnP is skipped
    QHostAddress bindAddress{QHostAddress::AnyIPv4};
    std::chrono::seconds natPmpLifetime{7200}; // RFC 6886's recommendation
    std::chrono::seconds upnpLease{3600};
    QString description;
    Timing timing;
    std::optional<Mapping> renew; // renews this mapping: same router, same protocol, same external port
};

enum class Outcome : quint8 {
    Mapped,
    NoRouter,  // nothing answered
    Refused,   // a router answered no
    Cancelled, // `cancel` was set before anything was mapped
};

struct MapResult
{
    Outcome outcome = Outcome::NoRouter;
    Mapping mapping; // Mapped
    QString reason;  // Refused: the router's answer, in words
};

// The default IPv4 gateway from the routing table (libnatpmp), or nullopt.
std::optional<QHostAddress> defaultGateway();

// Maps request.internalPort for TCP: NAT-PMP first (fast, one round trip), then UPnP. A NAT-PMP refusal still tries
// UPnP; the refusal is reported if UPnP finds nothing either. Once a mapping is made it is returned even if `cancel`
// was set meanwhile: the caller removes it.
MapResult mapPort(const MapRequest &request, const Cancel &cancel);

// Removes a mapping. True when the router confirmed it, or said there was no such mapping.
bool removeMapping(const Mapping &mapping, const QHostAddress &bindAddress, const Timing &timing,
                   const Cancel &cancel);

// True for addresses peers on the internet can't reach: private, shared (carrier-grade NAT), link-local, loopback or
// unspecified. A router whose own public address is one of these sits behind another NAT.
bool isUnroutable(const QHostAddress &address);

// The wire formats, for tests.
namespace detail {

inline constexpr qsizetype kDatagramMax = 2048;       // SSDP and NAT-PMP answers
inline constexpr qsizetype kHttpMax = qsizetype{256} * 1024; // a device description or SOAP answer, headers included
inline constexpr qsizetype kHttpHeaderMax = qsizetype{16} * 1024;

QByteArray natPmpAddressRequest();
// lifetime 0 with external port 0 removes the mapping.
QByteArray natPmpMapRequest(quint16 internalPort, quint16 externalPort, quint32 lifetime);

struct NatPmpAnswer
{
    quint8 opcode = 0; // 128 (address) or 130 (TCP mapping)
    quint16 result = 0;
    QHostAddress address; // opcode 128
    quint16 internalPort = 0;
    quint16 externalPort = 0;
    quint32 lifetime = 0;
};
std::optional<NatPmpAnswer> parseNatPmpAnswer(const QByteArray &datagram);
QString natPmpResultText(quint16 result);

QByteArray ssdpSearch(const QString &searchTarget, const Endpoint &to);
// The LOCATION of an SSDP answer, when it is an http URL on the sender's own address.
std::optional<QUrl> ssdpLocation(const QByteArray &datagram, const QHostAddress &sender);

struct IgdService
{
    QUrl controlUrl;
    QString serviceType;
};
// The WAN connection services of a device description, best first (WANIPConnection:2, :1, then WANPPPConnection:1);
// only those whose control URL is on the description's own host.
QList<IgdService> parseDescription(const QByteArray &xml, const QUrl &location);

QByteArray soapEnvelope(const QString &serviceType, const QString &action,
                        const QList<std::pair<QString, QString>> &arguments);
struct SoapAnswer
{
    bool ok = false;
    int errorCode = 0; // the UPnP error of a fault, 0 when none was given
    QString errorDescription;
    QHash<QString, QString> values; // the answer's arguments by name
};
SoapAnswer parseSoapAnswer(int httpStatus, const QByteArray &body);

struct HttpResponse
{
    int status = 0;
    QByteArray body;
};
struct HttpParse
{
    enum class State : quint8 {
        Incomplete,
        Complete,
        Malformed,
    };
    State state = State::Incomplete;
    HttpResponse response;
};
// Parses an HTTP/1.x answer received so far (Content-Length, chunked, or up to the close). `closed`: nothing more
// will come.
HttpParse parseHttpResponse(const QByteArray &raw, bool closed);

} // namespace detail

} // namespace selecta::core::portmap
