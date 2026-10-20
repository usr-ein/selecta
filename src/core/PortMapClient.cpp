// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PortMapClient.h"

#include <QDeadlineTimer>
#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QXmlStreamReader>
#include <QtEndian>

#include <natpmp.h>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::core::portmap {

namespace {

Q_LOGGING_CATEGORY(lcPortMap, "selecta.core.portmap")

constexpr std::chrono::milliseconds kSlice{100}; // the longest wait between two looks at `cancel`
constexpr qsizetype kMaxLocations = 4;           // devices tried from one SSDP search
constexpr int kAlternativePorts = 2;             // other external ports tried when ours is taken (UPnP)
constexpr int kMaxSoapValues = 64;

constexpr quint8 kOpAddress = 0;
constexpr quint8 kOpMapTcp = 2;
constexpr quint8 kOpAnswer = 128;

// UPnP errors (UPnP-gw-WANIPConnection-v2, §2.4.16).
constexpr int kNoSuchEntry = 714;
constexpr int kConflict = 718;
constexpr int kOnlyPermanentLeases = 725;

int sliceOf(const QDeadlineTimer &deadline)
{
    return static_cast<int>(std::clamp<qint64>(deadline.remainingTime(), 0, kSlice.count()));
}

QHostAddress ipv4(const QHostAddress &address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : address;
}

bool sameAddress(const QHostAddress &a, const QHostAddress &b)
{
    return ipv4(a) == ipv4(b);
}

// An IP-literal http URL, or nullopt: nothing is ever resolved by name.
std::optional<QHostAddress> hostOf(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != "http"_L1) {
        return std::nullopt;
    }
    const QHostAddress address(url.host());
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
        return std::nullopt;
    }
    return address;
}

// --- NAT-PMP ---------------------------------------------------------------------------------------------------------

// One request with RFC 6886's retransmissions: the answer from the gateway with the expected opcode, or nullopt.
std::optional<detail::NatPmpAnswer> natPmpExchange(const Endpoint &gateway, const QHostAddress &bindAddress,
                                                   const QByteArray &request, quint8 expectedOpcode,
                                                   const Timing &timing, const Cancel &cancel)
{
    QUdpSocket socket;
    if (!socket.bind(bindAddress, 0)) {
        qCDebug(lcPortMap) << "NAT-PMP: can't bind:" << socket.errorString();
        return std::nullopt;
    }
    std::chrono::milliseconds wait = timing.natPmpFirstWait;
    for (int attempt = 0; attempt < timing.natPmpTries && !cancel.load(); ++attempt, wait *= 2) {
        if (socket.writeDatagram(request, gateway.address, gateway.port) != request.size()) {
            return std::nullopt;
        }
        const QDeadlineTimer deadline(wait);
        while (!deadline.hasExpired() && !cancel.load()) {
            if (!socket.hasPendingDatagrams() && !socket.waitForReadyRead(sliceOf(deadline))) {
                continue;
            }
            while (socket.hasPendingDatagrams()) {
                const QNetworkDatagram datagram = socket.receiveDatagram(detail::kDatagramMax);
                // RFC 6886 §3.1: only the gateway's answers count.
                if (!sameAddress(datagram.senderAddress(), gateway.address) || datagram.senderPort() != gateway.port) {
                    continue;
                }
                const auto answer = detail::parseNatPmpAnswer(datagram.data());
                if (answer && answer->opcode == expectedOpcode) {
                    return answer;
                }
            }
        }
    }
    return std::nullopt;
}

struct NatPmpTry
{
    std::optional<Mapping> mapping;
    bool answered = false; // the gateway speaks NAT-PMP
    QString refusal;
};

NatPmpTry natPmpMap(const MapRequest &request, const Endpoint &gateway, quint16 externalPort, const Cancel &cancel)
{
    NatPmpTry result;
    QHostAddress external;
    if (!request.renew) {
        const auto address = natPmpExchange(gateway, request.bindAddress, detail::natPmpAddressRequest(),
                                            kOpAnswer | kOpAddress, request.timing, cancel);
        if (!address) {
            return result; // no NAT-PMP here
        }
        if (address->result != 0) {
            result.answered = address->result != 1; // 1: a PCP-only or otherwise unsupported version
            result.refusal = detail::natPmpResultText(address->result);
            return result;
        }
        external = address->address;
    } else {
        external = request.renew->externalAddress;
    }
    result.answered = true;
    if (cancel.load()) {
        return result;
    }
    const auto lifetime = static_cast<quint32>(request.natPmpLifetime.count());
    const auto answer =
        natPmpExchange(gateway, request.bindAddress, detail::natPmpMapRequest(request.internalPort, externalPort, lifetime),
                       kOpAnswer | kOpMapTcp, request.timing, cancel);
    if (!answer || answer->internalPort != request.internalPort) {
        result.refusal = QObject::tr("The router didn't answer the NAT-PMP mapping request.");
        return result;
    }
    if (answer->result != 0 || answer->externalPort == 0 || answer->lifetime == 0) {
        result.refusal = answer->result != 0 ? detail::natPmpResultText(answer->result)
                                             : QObject::tr("The router answered NAT-PMP with an empty mapping.");
        return result;
    }
    Mapping mapping;
    mapping.protocol = Protocol::NatPmp;
    mapping.internalPort = request.internalPort;
    mapping.externalPort = answer->externalPort;
    mapping.externalAddress = external;
    mapping.lifetime = std::chrono::seconds(answer->lifetime);
    mapping.gateway = gateway;
    result.mapping = mapping;
    return result;
}

// --- HTTP and SOAP ---------------------------------------------------------------------------------------------------

struct HttpExchange
{
    std::optional<detail::HttpResponse> response;
    QHostAddress localAddress; // our side of the connection: the address the router sees
};

HttpExchange httpExchange(const QUrl &url, const QByteArray &method, const QByteArray &extraHeaders,
                          const QByteArray &body, std::chrono::milliseconds timeout, const Cancel &cancel)
{
    HttpExchange result;
    const auto host = hostOf(url);
    if (!host || url.port() <= 0) {
        return result;
    }
    const QDeadlineTimer deadline(timeout);
    QTcpSocket socket;
    socket.connectToHost(*host, static_cast<quint16>(url.port()));
    while (socket.state() != QAbstractSocket::ConnectedState) {
        if (cancel.load() || deadline.hasExpired() || socket.state() == QAbstractSocket::UnconnectedState) {
            return result;
        }
        socket.waitForConnected(sliceOf(deadline));
    }
    result.localAddress = ipv4(socket.localAddress());
    QByteArray path = url.path(QUrl::FullyEncoded).toUtf8();
    if (path.isEmpty()) {
        path = "/"_ba;
    }
    if (url.hasQuery()) {
        path += '?' + url.query(QUrl::FullyEncoded).toUtf8();
    }
    QByteArray head = method + ' ' + path + " HTTP/1.1\r\nHost: "_ba + host->toString().toUtf8() + ':' +
                      QByteArray::number(url.port()) + "\r\nConnection: close\r\n"_ba + extraHeaders;
    if (!body.isEmpty()) {
        head += "Content-Length: "_ba + QByteArray::number(body.size()) + "\r\n"_ba;
    }
    head += "\r\n"_ba;
    socket.write(head + body);
    QByteArray received;
    for (;;) {
        if (cancel.load() || deadline.hasExpired()) {
            return result;
        }
        const bool closed = socket.state() == QAbstractSocket::UnconnectedState;
        if (socket.bytesAvailable() > 0) {
            received += socket.read(detail::kHttpMax + 1 - received.size());
            if (received.size() > detail::kHttpMax) {
                qCDebug(lcPortMap) << "UPnP: an answer over" << detail::kHttpMax << "bytes from" << url.host();
                return result;
            }
        }
        const detail::HttpParse parse = detail::parseHttpResponse(received, closed && socket.bytesAvailable() == 0);
        if (parse.state == detail::HttpParse::State::Complete) {
            result.response = parse.response;
            return result;
        }
        if (parse.state == detail::HttpParse::State::Malformed || closed) {
            return result;
        }
        if (socket.bytesToWrite() > 0) {
            socket.waitForBytesWritten(sliceOf(deadline));
        } else {
            socket.waitForReadyRead(sliceOf(deadline));
        }
    }
}

struct SoapCall
{
    std::optional<detail::SoapAnswer> answer; // nullopt: no usable HTTP answer
    QHostAddress localAddress;
};

SoapCall soap(const QUrl &controlUrl, const QString &serviceType, const QString &action,
              const QList<std::pair<QString, QString>> &arguments, const Timing &timing, const Cancel &cancel)
{
    const QByteArray headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \""_ba +
                               serviceType.toUtf8() + '#' + action.toUtf8() + "\"\r\n"_ba;
    const HttpExchange exchange = httpExchange(controlUrl, "POST"_ba, headers,
                                               detail::soapEnvelope(serviceType, action, arguments),
                                               timing.httpTimeout, cancel);
    SoapCall call;
    call.localAddress = exchange.localAddress;
    if (exchange.response) {
        call.answer = detail::parseSoapAnswer(exchange.response->status, exchange.response->body);
    }
    return call;
}

QString upnpRefusal(const detail::SoapAnswer &answer)
{
    if (answer.errorCode == 0) {
        return QObject::tr("The router refused the UPnP request.");
    }
    return QObject::tr("The router refused (UPnP error %1%2).")
        .arg(answer.errorCode)
        .arg(answer.errorDescription.isEmpty() ? QString() : u": "_s + answer.errorDescription.left(120));
}

// --- SSDP ------------------------------------------------------------------------------------------------------------

QList<QUrl> ssdpDiscover(const Endpoint &to, const QHostAddress &bindAddress, std::chrono::milliseconds wait,
                         const Cancel &cancel)
{
    static const std::array<QString, 3> kTargets{
        u"urn:schemas-upnp-org:device:InternetGatewayDevice:1"_s,
        u"urn:schemas-upnp-org:device:InternetGatewayDevice:2"_s,
        u"urn:schemas-upnp-org:service:WANIPConnection:1"_s,
    };
    QList<QUrl> found;
    QUdpSocket socket;
    if (!socket.bind(bindAddress, 0)) {
        qCDebug(lcPortMap) << "SSDP: can't bind:" << socket.errorString();
        return found;
    }
    if (to.address.isMulticast()) {
        socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 2); // UPnP Device Architecture 2.0, §1.1.2
    }
    const auto send = [&] {
        for (const QString &target : kTargets) {
            socket.writeDatagram(detail::ssdpSearch(target, to), to.address, to.port);
        }
    };
    send();
    bool sentAgain = false;
    QDeadlineTimer deadline(wait);
    while (!deadline.hasExpired() && !cancel.load()) {
        if (!sentAgain && found.isEmpty() && deadline.remainingTime() < wait.count() / 2) {
            send(); // once more, in case the first datagrams were lost
            sentAgain = true;
        }
        if (!socket.hasPendingDatagrams() && !socket.waitForReadyRead(sliceOf(deadline))) {
            continue;
        }
        while (socket.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = socket.receiveDatagram(detail::kDatagramMax);
            const auto location = detail::ssdpLocation(datagram.data(), datagram.senderAddress());
            if (!location || found.contains(*location)) {
                continue;
            }
            found.append(*location);
            if (found.size() == 1) {
                deadline = QDeadlineTimer(std::min<qint64>(deadline.remainingTime(), 300)); // a moment for others
            }
            if (found.size() >= kMaxLocations) {
                return found;
            }
        }
    }
    return found;
}

// --- UPnP ------------------------------------------------------------------------------------------------------------

struct Igd
{
    detail::IgdService service;
    QHostAddress externalAddress;
    QHostAddress internalClient;
};

// The first WAN connection service that answers, preferring one with a public address.
std::optional<Igd> findIgd(const MapRequest &request, const Cancel &cancel)
{
    const QList<QUrl> locations = ssdpDiscover(*request.ssdp, request.bindAddress, request.timing.ssdpWait, cancel);
    std::optional<Igd> fallback;
    for (const QUrl &location : locations) {
        if (cancel.load()) {
            return std::nullopt;
        }
        const HttpExchange description = httpExchange(location, "GET"_ba, {}, {}, request.timing.httpTimeout, cancel);
        if (!description.response || description.response->status != 200) {
            qCDebug(lcPortMap) << "UPnP: no description at" << location.toString();
            continue;
        }
        for (const detail::IgdService &service : detail::parseDescription(description.response->body, location)) {
            if (cancel.load()) {
                return std::nullopt;
            }
            const SoapCall call =
                soap(service.controlUrl, service.serviceType, u"GetExternalIPAddress"_s, {}, request.timing, cancel);
            if (!call.answer || !call.answer->ok) {
                continue;
            }
            Igd igd{service, QHostAddress(call.answer->values.value(u"NewExternalIPAddress"_s)), call.localAddress};
            if (!igd.externalAddress.isNull() && igd.externalAddress != QHostAddress(QHostAddress::AnyIPv4)) {
                return igd; // a connected WAN service
            }
            if (!fallback) {
                fallback = igd;
            }
        }
    }
    return fallback;
}

struct UpnpTry
{
    std::optional<Mapping> mapping;
    bool answered = false;
    QString refusal;
};

UpnpTry upnpMap(const MapRequest &request, quint16 externalPort, const Cancel &cancel)
{
    UpnpTry result;
    Igd igd;
    if (request.renew) {
        igd.service = {request.renew->controlUrl, request.renew->serviceType};
        igd.externalAddress = request.renew->externalAddress;
        igd.internalClient = request.renew->internalClient;
    } else {
        const auto found = findIgd(request, cancel);
        if (!found) {
            return result;
        }
        igd = *found;
    }
    if (igd.internalClient.isNull()) {
        return result;
    }
    const QString internalPort = QString::number(request.internalPort);
    const QString client = igd.internalClient.toString();
    std::chrono::seconds lease = request.upnpLease;
    QList<quint16> ports{externalPort};
    for (int i = 0; i < kAlternativePorts && !request.renew; ++i) {
        ports.append(static_cast<quint16>(QRandomGenerator::global()->bounded(49152, 65536))); // dynamic range
    }
    for (qsizetype i = 0; i < ports.size(); ++i) {
        if (cancel.load()) {
            return result;
        }
        const QString port = QString::number(ports.at(i));
        const auto add = [&] {
            return soap(igd.service.controlUrl, igd.service.serviceType, u"AddPortMapping"_s,
                        {{u"NewRemoteHost"_s, QString()},
                         {u"NewExternalPort"_s, port},
                         {u"NewProtocol"_s, u"TCP"_s},
                         {u"NewInternalPort"_s, internalPort},
                         {u"NewInternalClient"_s, client},
                         {u"NewEnabled"_s, u"1"_s},
                         {u"NewPortMappingDescription"_s, request.description},
                         {u"NewLeaseDuration"_s, QString::number(lease.count())}},
                        request.timing, cancel);
        };
        SoapCall call = add();
        if (call.answer && !call.answer->ok && call.answer->errorCode == kOnlyPermanentLeases && lease.count() != 0) {
            lease = std::chrono::seconds(0); // an IGD:1 that only keeps mappings until they are removed
            call = add();
        }
        result.answered = true; // a router that answered the search (or mapped before), at least
        if (!call.answer) {
            result.refusal = QObject::tr("The router didn't answer the UPnP mapping request.");
            return result;
        }
        bool mapped = call.answer->ok;
        if (!mapped && call.answer->errorCode == kConflict) {
            // Taken: by us (a mapping left from an earlier run), or by another device.
            const SoapCall entry = soap(igd.service.controlUrl, igd.service.serviceType,
                                        u"GetSpecificPortMappingEntry"_s,
                                        {{u"NewRemoteHost"_s, QString()},
                                         {u"NewExternalPort"_s, port},
                                         {u"NewProtocol"_s, u"TCP"_s}},
                                        request.timing, cancel);
            mapped = entry.answer && entry.answer->ok &&
                     sameAddress(QHostAddress(entry.answer->values.value(u"NewInternalClient"_s)), igd.internalClient) &&
                     entry.answer->values.value(u"NewInternalPort"_s) == internalPort;
            if (!mapped) {
                result.refusal = QObject::tr("Port %1 is already open on the router for another device.").arg(port);
                continue;
            }
        }
        if (!mapped) {
            result.refusal = upnpRefusal(*call.answer);
            return result;
        }
        Mapping mapping;
        mapping.protocol = Protocol::Upnp;
        mapping.internalPort = request.internalPort;
        mapping.externalPort = ports.at(i);
        mapping.externalAddress = igd.externalAddress;
        mapping.lifetime = lease;
        mapping.controlUrl = igd.service.controlUrl;
        mapping.serviceType = igd.service.serviceType;
        mapping.internalClient = igd.internalClient;
        result.mapping = mapping;
        return result;
    }
    return result;
}

} // namespace

Endpoint ssdpMulticast()
{
    return {QHostAddress(u"239.255.255.250"_s), 1900};
}

std::optional<QHostAddress> defaultGateway()
{
    // initnatpmp() reads the routing table and connects a UDP socket (which sends nothing); only the address is kept.
    natpmp_t natpmp{};
    const int status = initnatpmp(&natpmp, 0, 0);
    if (status != NATPMP_ERR_SOCKETERROR) {
        closenatpmp(&natpmp);
    }
    if (status != 0 || natpmp.gateway == 0) {
        return std::nullopt;
    }
    return QHostAddress(qFromBigEndian<quint32>(static_cast<quint32>(natpmp.gateway)));
}

bool isUnroutable(const QHostAddress &address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    if (!ok) {
        return address.isNull() || address.isLoopback() || address.isLinkLocal() || address.isPrivateUse();
    }
    const auto in = [v4](quint32 network, int bits) { return (v4 >> (32 - bits)) == (network >> (32 - bits)); };
    return v4 == 0 || in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) || in(0x64400000, 10) ||
           in(0xA9FE0000, 16) || in(0x7F000000, 8);
}

MapResult mapPort(const MapRequest &request, const Cancel &cancel)
{
    MapResult result;
    const quint16 wanted = request.renew ? request.renew->externalPort
                                         : (request.externalPort != 0 ? request.externalPort : request.internalPort);
    const bool tryNatPmp = request.renew ? request.renew->protocol == Protocol::NatPmp : request.natPmpGateway.has_value();
    const bool tryUpnp = request.renew ? request.renew->protocol == Protocol::Upnp : request.ssdp.has_value();
    QString refusal;
    if (tryNatPmp && !cancel.load()) {
        const Endpoint gateway = request.renew ? request.renew->gateway : *request.natPmpGateway;
        const NatPmpTry natPmp = natPmpMap(request, gateway, wanted, cancel);
        if (natPmp.mapping) {
            result.outcome = Outcome::Mapped;
            result.mapping = *natPmp.mapping;
            return result;
        }
        if (natPmp.answered) {
            refusal = natPmp.refusal;
        }
    }
    if (tryUpnp && !cancel.load()) {
        const UpnpTry upnp = upnpMap(request, wanted, cancel);
        if (upnp.mapping) {
            result.outcome = Outcome::Mapped;
            result.mapping = *upnp.mapping;
            return result;
        }
        if (upnp.answered) {
            refusal = upnp.refusal;
        }
    }
    if (cancel.load()) {
        result.outcome = Outcome::Cancelled;
    } else if (!refusal.isEmpty()) {
        result.outcome = Outcome::Refused;
        result.reason = refusal;
    }
    return result;
}

bool removeMapping(const Mapping &mapping, const QHostAddress &bindAddress, const Timing &timing,
                   const Cancel &cancel)
{
    if (mapping.protocol == Protocol::NatPmp) {
        // RFC 6886 §3.4: lifetime 0 and external port 0 delete the mapping of this internal port.
        const auto answer = natPmpExchange(mapping.gateway, bindAddress,
                                           detail::natPmpMapRequest(mapping.internalPort, 0, 0),
                                           kOpAnswer | kOpMapTcp, timing, cancel);
        return answer && answer->result == 0;
    }
    const SoapCall call = soap(mapping.controlUrl, mapping.serviceType, u"DeletePortMapping"_s,
                               {{u"NewRemoteHost"_s, QString()},
                                {u"NewExternalPort"_s, QString::number(mapping.externalPort)},
                                {u"NewProtocol"_s, u"TCP"_s}},
                               timing, cancel);
    return call.answer && (call.answer->ok || call.answer->errorCode == kNoSuchEntry);
}

namespace detail {

QByteArray natPmpAddressRequest()
{
    return QByteArray(2, '\0'); // version 0, opcode 0
}

QByteArray natPmpMapRequest(quint16 internalPort, quint16 externalPort, quint32 lifetime)
{
    QByteArray request(12, '\0');
    auto *bytes = reinterpret_cast<uchar *>(request.data());
    bytes[1] = kOpMapTcp;
    qToBigEndian(internalPort, bytes + 4);
    qToBigEndian(externalPort, bytes + 6);
    qToBigEndian(lifetime, bytes + 8);
    return request;
}

std::optional<NatPmpAnswer> parseNatPmpAnswer(const QByteArray &datagram)
{
    const auto *bytes = reinterpret_cast<const uchar *>(datagram.constData());
    if (datagram.size() < 8 || bytes[0] != 0 || (bytes[1] & kOpAnswer) == 0) {
        return std::nullopt;
    }
    NatPmpAnswer answer;
    answer.opcode = bytes[1];
    answer.result = qFromBigEndian<quint16>(bytes + 2);
    if (answer.opcode == (kOpAnswer | kOpAddress)) {
        if (datagram.size() < 12) {
            return std::nullopt;
        }
        answer.address = QHostAddress(qFromBigEndian<quint32>(bytes + 8));
        return answer;
    }
    if (answer.opcode == (kOpAnswer | kOpMapTcp)) {
        if (datagram.size() < 16) {
            return std::nullopt;
        }
        answer.internalPort = qFromBigEndian<quint16>(bytes + 8);
        answer.externalPort = qFromBigEndian<quint16>(bytes + 10);
        answer.lifetime = qFromBigEndian<quint32>(bytes + 12);
        return answer;
    }
    return std::nullopt;
}

QString natPmpResultText(quint16 result)
{
    switch (result) {
    case 0:
        return {};
    case 1:
        return QObject::tr("The router doesn't support this version of NAT-PMP.");
    case 2:
        return QObject::tr("The router refused: port mapping is turned off or not allowed (NAT-PMP).");
    case 3:
        return QObject::tr("The router has no internet connection yet (NAT-PMP).");
    case 4:
        return QObject::tr("The router has no room for another port mapping (NAT-PMP).");
    case 5:
        return QObject::tr("The router doesn't support TCP port mapping (NAT-PMP).");
    default:
        return QObject::tr("The router refused (NAT-PMP result %1).").arg(result);
    }
}

QByteArray ssdpSearch(const QString &searchTarget, const Endpoint &to)
{
    return "M-SEARCH * HTTP/1.1\r\nHOST: "_ba + to.address.toString().toUtf8() + ':' + QByteArray::number(to.port) +
           "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: "_ba + searchTarget.toUtf8() + "\r\n\r\n"_ba;
}

std::optional<QUrl> ssdpLocation(const QByteArray &datagram, const QHostAddress &sender)
{
    const QList<QByteArray> lines = datagram.split('\n');
    if (lines.isEmpty() || !lines.first().trimmed().toUpper().startsWith("HTTP/1.1 200"_ba)) {
        return std::nullopt;
    }
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray &line = lines.at(i);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0 || line.left(colon).trimmed().toUpper() != "LOCATION"_ba) {
            continue;
        }
        const QUrl url(QString::fromLatin1(line.mid(colon + 1).trimmed()), QUrl::StrictMode);
        const auto host = hostOf(url);
        if (!host || url.port() <= 0 || !sameAddress(*host, sender)) {
            return std::nullopt;
        }
        return url;
    }
    return std::nullopt;
}

QList<IgdService> parseDescription(const QByteArray &xml, const QUrl &location)
{
    static const std::array<QString, 3> kPreferred{
        u"urn:schemas-upnp-org:service:WANIPConnection:2"_s,
        u"urn:schemas-upnp-org:service:WANIPConnection:1"_s,
        u"urn:schemas-upnp-org:service:WANPPPConnection:1"_s,
    };
    QXmlStreamReader reader(xml);
    QString urlBase;
    QList<std::pair<QString, QString>> services; // type, control URL
    QString type;
    QString control;
    bool inService = false;
    while (!reader.atEnd() && services.size() < 32) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringView name = reader.name();
            if (name == "service"_L1) {
                inService = true;
                type.clear();
                control.clear();
            } else if (name == "URLBase"_L1) {
                urlBase = reader.readElementText().trimmed();
            } else if (inService && name == "serviceType"_L1) {
                type = reader.readElementText().trimmed();
            } else if (inService && name == "controlURL"_L1) {
                control = reader.readElementText().trimmed();
            }
        } else if (reader.isEndElement() && reader.name() == "service"_L1) {
            inService = false;
            services.append({type, control});
        }
    }
    if (reader.hasError() && services.isEmpty()) {
        return {};
    }
    QUrl base = location;
    if (const QUrl declared(urlBase); !urlBase.isEmpty() && hostOf(declared) && declared.host() == location.host()) {
        base = declared;
    }
    QList<IgdService> found;
    for (const QString &preferred : kPreferred) {
        for (const auto &[serviceType, controlUrl] : std::as_const(services)) {
            if (serviceType != preferred || controlUrl.isEmpty()) {
                continue;
            }
            const QUrl url = base.resolved(QUrl(controlUrl));
            // The control URL stays on the device that answered the search.
            if (hostOf(url) && url.host() == location.host() && url.port() > 0) {
                found.append({url, serviceType});
            }
        }
    }
    return found.mid(0, 4);
}

QByteArray soapEnvelope(const QString &serviceType, const QString &action,
                        const QList<std::pair<QString, QString>> &arguments)
{
    QString body = u"<?xml version=\"1.0\"?>\r\n"
                   u"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                   u"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                   u"<u:%1 xmlns:u=\"%2\">"_s.arg(action, serviceType.toHtmlEscaped());
    for (const auto &[name, value] : arguments) {
        body += u"<%1>%2</%1>"_s.arg(name, value.toHtmlEscaped());
    }
    body += u"</u:%1></s:Body></s:Envelope>\r\n"_s.arg(action);
    return body.toUtf8();
}

SoapAnswer parseSoapAnswer(int httpStatus, const QByteArray &body)
{
    SoapAnswer answer;
    QXmlStreamReader reader(body);
    bool fault = false;
    QString text;
    bool leaf = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == "Fault"_L1) {
                fault = true;
            }
            text.clear();
            leaf = true;
        } else if (reader.isCharacters()) {
            if (text.size() < 1024) {
                text += reader.text();
            }
        } else if (reader.isEndElement()) {
            if (leaf && answer.values.size() < kMaxSoapValues && !answer.values.contains(reader.name().toString())) {
                answer.values.insert(reader.name().toString(), text.trimmed());
            }
            leaf = false;
        }
    }
    const bool readable = !reader.hasError();
    if (fault || httpStatus != 200) {
        answer.errorCode = answer.values.value(u"errorCode"_s).toInt();
        answer.errorDescription = answer.values.value(u"errorDescription"_s);
        return answer;
    }
    answer.ok = readable;
    return answer;
}

HttpParse parseHttpResponse(const QByteArray &raw, bool closed)
{
    using State = HttpParse::State;
    HttpParse parse;
    const qsizetype headerEnd = raw.indexOf("\r\n\r\n"_ba);
    if (headerEnd < 0) {
        parse.state = closed || raw.size() > kHttpHeaderMax ? State::Malformed : State::Incomplete;
        return parse;
    }
    if (headerEnd > kHttpHeaderMax) {
        parse.state = State::Malformed;
        return parse;
    }
    const QList<QByteArray> lines = raw.left(headerEnd).split('\n');
    const QList<QByteArray> status = lines.first().trimmed().split(' ');
    bool ok = false;
    parse.response.status = status.size() >= 2 ? status.at(1).toInt(&ok) : 0;
    if (!status.first().startsWith("HTTP/1."_ba) || !ok || parse.response.status < 100 ||
        parse.response.status > 599) {
        parse.state = State::Malformed;
        return parse;
    }
    qint64 contentLength = -1;
    bool chunked = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray &line = lines.at(i);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length"_ba) {
            contentLength = value.toLongLong(&ok);
            if (!ok || contentLength < 0 || contentLength > kHttpMax) {
                parse.state = State::Malformed;
                return parse;
            }
        } else if (name == "transfer-encoding"_ba && value.toLower().contains("chunked"_ba)) {
            chunked = true;
        }
    }
    const QByteArray body = raw.mid(headerEnd + 4);
    if (chunked) {
        QByteArray decoded;
        qsizetype at = 0;
        for (;;) {
            const qsizetype lineEnd = body.indexOf("\r\n"_ba, at);
            if (lineEnd < 0) {
                parse.state = closed ? State::Malformed : State::Incomplete;
                return parse;
            }
            QByteArray size = body.mid(at, lineEnd - at);
            if (const qsizetype extension = size.indexOf(';'); extension >= 0) {
                size.truncate(extension);
            }
            const qint64 length = size.trimmed().toLongLong(&ok, 16);
            if (!ok || length < 0 || length > kHttpMax) {
                parse.state = State::Malformed;
                return parse;
            }
            at = lineEnd + 2;
            if (length == 0) {
                parse.response.body = decoded;
                parse.state = State::Complete; // trailers, if any, are ignored
                return parse;
            }
            if (body.size() < at + length + 2) {
                parse.state = closed ? State::Malformed : State::Incomplete;
                return parse;
            }
            decoded += body.mid(at, length);
            at += length + 2;
        }
    }
    if (contentLength >= 0) {
        if (body.size() < contentLength) {
            parse.state = closed ? State::Malformed : State::Incomplete;
            return parse;
        }
        parse.response.body = body.left(contentLength);
        parse.state = State::Complete;
        return parse;
    }
    if (!closed) {
        return parse; // the body runs to the close
    }
    parse.response.body = body;
    parse.state = State::Complete;
    return parse;
}

} // namespace detail

} // namespace selecta::core::portmap
