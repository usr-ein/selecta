// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loopback stand-ins for a home router's port-mapping services (NET-3): a NAT-PMP responder, and an SSDP responder with
// the IGD's HTTP server (device description and SOAP control). Tests never touch the real router. Both live on the
// test's thread and answer from its event loop.

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QNetworkDatagram>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QXmlStreamReader>
#include <QtEndian>

namespace selecta::core::testing {

using namespace Qt::StringLiterals;

// A NAT-PMP gateway (RFC 6886) on 127.0.0.1.
//
// A client that hears nothing back sends the same request again from the same socket (RFC 6886 §3.1), and a fake
// that answers from a busy test thread may get both copies before it answers either. The copy is answered like the
// first but is not a new request: mapRequests() and removals() leave it out. It is told by its bytes and sender port,
// both the same as the last datagram from that port: an exchange from a new socket always counts, unless the system
// hands that socket the same ephemeral port and the request is the same (renewing a mapping), which is rare enough
// to only ever undercount by one.
class FakeNatPmp
{
public:
    struct Request
    {
        quint8 opcode = 0;
        quint16 internalPort = 0;
        quint16 externalPort = 0;
        quint32 lifetime = 0;
        bool retransmission = false; // the same datagram again from the same socket
    };

    FakeNatPmp()
    {
        m_socket.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&m_socket, &QUdpSocket::readyRead, &m_socket, [this] { answer(); });
    }

    ~FakeNatPmp() = default;
    FakeNatPmp(const FakeNatPmp &) = delete;
    FakeNatPmp &operator=(const FakeNatPmp &) = delete;
    FakeNatPmp(FakeNatPmp &&) = delete;
    FakeNatPmp &operator=(FakeNatPmp &&) = delete;

    quint16 port() const { return m_socket.localPort(); }
    int mapRequests() const
    {
        int n = 0;
        for (const Request &request : requests) {
            n += request.opcode == 2 && request.lifetime != 0 && !request.retransmission ? 1 : 0;
        }
        return n;
    }
    int removals() const
    {
        int n = 0;
        for (const Request &request : requests) {
            n += request.opcode == 2 && request.lifetime == 0 && !request.retransmission ? 1 : 0;
        }
        return n;
    }

    // Behaviour.
    bool silent = false;                       // answers nothing (a gateway without NAT-PMP)
    quint16 addressResult = 0;                 // the external-address request's result code
    quint16 mapResult = 0;                     // the mapping request's result code
    quint32 grantedLifetime = 0;               // 0: as asked
    QHostAddress external{u"203.0.113.7"_s};   // the router's public address
    QMap<quint16, quint16> takenExternalPorts; // external port → used by someone else: the next free one is given
    bool loseFirstAnswers = false;             // a mapping request is carried out, but only its copy is answered

    // What it saw and holds.
    int received = 0; // datagrams, answered or not
    QList<Request> requests;
    QMap<quint16, quint16> mappings; // internal → external

private:
    void answer()
    {
        while (m_socket.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = m_socket.receiveDatagram();
            const QByteArray in = datagram.data();
            const auto *bytes = reinterpret_cast<const uchar *>(in.constData());
            ++received;
            if (in.size() < 2 || bytes[0] != 0 || silent) {
                continue;
            }
            const quint16 from = datagram.senderPort();
            Request request;
            request.opcode = bytes[1];
            request.retransmission = m_lastFrom.value(from) == in;
            m_lastFrom.insert(from, in);
            QByteArray out;
            if (request.opcode == 0 && in.size() == 2) {
                out = QByteArray(12, '\0');
                auto *o = reinterpret_cast<uchar *>(out.data());
                o[1] = 128;
                qToBigEndian<quint16>(addressResult, o + 2);
                qToBigEndian<quint32>(1000, o + 4);
                qToBigEndian<quint32>(external.toIPv4Address(), o + 8);
            } else if (request.opcode == 2 && in.size() == 12) {
                request.internalPort = qFromBigEndian<quint16>(bytes + 4);
                request.externalPort = qFromBigEndian<quint16>(bytes + 6);
                request.lifetime = qFromBigEndian<quint32>(bytes + 8);
                quint16 mapped = 0;
                quint32 lifetime = 0;
                if (mapResult == 0 && request.lifetime == 0) {
                    mappings.remove(request.internalPort);
                } else if (mapResult == 0) {
                    mapped = request.externalPort != 0 ? request.externalPort : request.internalPort;
                    while (takenExternalPorts.contains(mapped)) {
                        ++mapped;
                    }
                    mappings.insert(request.internalPort, mapped);
                    lifetime = grantedLifetime != 0 ? grantedLifetime : request.lifetime;
                }
                out = QByteArray(16, '\0');
                auto *o = reinterpret_cast<uchar *>(out.data());
                o[1] = 130;
                qToBigEndian<quint16>(mapResult, o + 2);
                qToBigEndian<quint32>(1000, o + 4);
                qToBigEndian<quint16>(request.internalPort, o + 8);
                qToBigEndian<quint16>(mapped, o + 10);
                qToBigEndian<quint32>(lifetime, o + 12);
            } else {
                continue;
            }
            requests.append(request);
            if (loseFirstAnswers && request.opcode == 2 && !request.retransmission) {
                continue; // lost on the way back
            }
            m_socket.writeDatagram(out, datagram.senderAddress(), from);
        }
    }

    QUdpSocket m_socket;
    QHash<quint16, QByteArray> m_lastFrom; // the last datagram from each sender port (all on loopback)
};

// An Internet Gateway Device: SSDP answers on a loopback UDP port and an HTTP server for its description and SOAP.
class FakeIgd
{
public:
    struct Call
    {
        QString action;
        QHash<QString, QString> arguments;
    };

    FakeIgd()
    {
        m_ssdp.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&m_ssdp, &QUdpSocket::readyRead, &m_ssdp, [this] { answerSearch(); });
        m_http.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_http, &QTcpServer::newConnection, &m_http, [this] {
            while (QTcpSocket *socket = m_http.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { serve(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    ~FakeIgd()
    {
        // Connections still open die with the server, after our members: they mustn't call back into us.
        const QList<QTcpSocket *> sockets = m_http.findChildren<QTcpSocket *>();
        for (const QTcpSocket *const socket : sockets) {
            delete socket;
        }
    }
    FakeIgd(const FakeIgd &) = delete;
    FakeIgd &operator=(const FakeIgd &) = delete;
    FakeIgd(FakeIgd &&) = delete;
    FakeIgd &operator=(FakeIgd &&) = delete;

    quint16 ssdpPort() const { return m_ssdp.localPort(); }
    quint16 httpPort() const { return m_http.serverPort(); }
    QList<Call> callsOf(const QString &action) const
    {
        QList<Call> found;
        for (const Call &call : calls) {
            if (call.action == action) {
                found.append(call);
            }
        }
        return found;
    }

    // Behaviour.
    bool chunked = false;             // sends the description in chunks
    int addPortMappingError = 0;      // a UPnP error for every AddPortMapping (606: not authorised…)
    bool permanentLeasesOnly = false; // 725 for a lease other than 0
    QString externalAddress = u"203.0.113.9"_s;
    QString locationHost;                // the host in LOCATION; default: 127.0.0.1
    QMap<quint16, QString> otherDevices; // external port → another device's mapping (718 for us)

    // What it saw and holds.
    QList<Call> calls;
    int searches = 0;
    QMap<quint16, std::pair<QString, quint16>> mappings; // external port → internal client and port

private:
    void answerSearch()
    {
        while (m_ssdp.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = m_ssdp.receiveDatagram();
            if (!datagram.data().startsWith("M-SEARCH")) {
                continue;
            }
            ++searches;
            const QString host = locationHost.isEmpty() ? u"127.0.0.1"_s : locationHost;
            const QByteArray reply = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: "
                                     "urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: http://"_ba +
                                     host.toUtf8() + ':' + QByteArray::number(httpPort()) +
                                     "/rootDesc.xml\r\nSERVER: Fake/1.0 UPnP/1.1 FakeIgd/1.0\r\n\r\n"_ba;
            m_ssdp.writeDatagram(reply, datagram.senderAddress(), datagram.senderPort());
        }
    }

    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const QByteArray head = buffer.left(headerEnd);
        qsizetype length = 0;
        QByteArray soapAction;
        const QList<QByteArray> lines = head.split('\n');
        for (const QByteArray &line : lines) {
            const QByteArray lower = line.toLower();
            if (lower.startsWith("content-length:")) {
                length = line.mid(15).trimmed().toLongLong();
            } else if (lower.startsWith("soapaction:")) {
                soapAction = line.mid(11).trimmed();
            }
        }
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        const QByteArray body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        if (head.startsWith("GET /rootDesc.xml")) {
            reply(socket, 200, description(), chunked);
        } else if (head.startsWith("POST /ctl/IPConn")) {
            const qsizetype hash = soapAction.indexOf('#');
            const QString action = QString::fromUtf8(soapAction.mid(hash + 1)).remove(u'"');
            control(socket, action, arguments(body));
        } else {
            reply(socket, 404, "not here"_ba);
        }
    }

    static QHash<QString, QString> arguments(const QByteArray &body)
    {
        QHash<QString, QString> found;
        QXmlStreamReader reader(body);
        int depth = 0;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement()) {
                ++depth;
                if (depth == 4) { // Envelope / Body / action / argument
                    const QString name = reader.name().toString();
                    found.insert(name, reader.readElementText());
                    --depth;
                }
            } else if (reader.isEndElement()) {
                --depth;
            }
        }
        return found;
    }

    void control(QTcpSocket *socket, const QString &action, const QHash<QString, QString> &args)
    {
        calls.append({action, args});
        const quint16 port = args.value(u"NewExternalPort"_s).toUShort();
        if (action == "GetExternalIPAddress"_L1) {
            ok(socket, action, {{u"NewExternalIPAddress"_s, externalAddress}});
        } else if (action == "AddPortMapping"_L1) {
            if (addPortMappingError != 0) {
                fault(socket, addPortMappingError, u"Action not authorized"_s);
            } else if (permanentLeasesOnly && args.value(u"NewLeaseDuration"_s) != "0"_L1) {
                fault(socket, 725, u"OnlyPermanentLeasesSupported"_s);
            } else if (otherDevices.contains(port)) {
                fault(socket, 718, u"ConflictInMappingEntry"_s);
            } else {
                mappings.insert(port,
                                {args.value(u"NewInternalClient"_s), args.value(u"NewInternalPort"_s).toUShort()});
                ok(socket, action, {});
            }
        } else if (action == "GetSpecificPortMappingEntry"_L1) {
            if (otherDevices.contains(port)) {
                ok(socket, action,
                   {{u"NewInternalPort"_s, args.value(u"NewExternalPort"_s)},
                    {u"NewInternalClient"_s, otherDevices.value(port)}});
            } else if (mappings.contains(port)) {
                ok(socket, action,
                   {{u"NewInternalPort"_s, QString::number(mappings.value(port).second)},
                    {u"NewInternalClient"_s, mappings.value(port).first}});
            } else {
                fault(socket, 714, u"NoSuchEntryInArray"_s);
            }
        } else if (action == "DeletePortMapping"_L1) {
            if (mappings.remove(port) > 0) {
                ok(socket, action, {});
            } else {
                fault(socket, 714, u"NoSuchEntryInArray"_s);
            }
        } else {
            fault(socket, 401, u"Invalid Action"_s);
        }
    }

    static void ok(QTcpSocket *socket, const QString &action, const QList<std::pair<QString, QString>> &values)
    {
        QString xml =
            u"<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            u"<s:Body><u:%1Response xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"_s.arg(action);
        for (const auto &[name, value] : values) {
            xml += u"<%1>%2</%1>"_s.arg(name, value);
        }
        xml += u"</u:%1Response></s:Body></s:Envelope>"_s.arg(action);
        reply(socket, 200, xml.toUtf8());
    }

    static void fault(QTcpSocket *socket, int code, const QString &description)
    {
        const QString xml =
            u"<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
            u"<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
            u"<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%1</errorCode>"
            u"<errorDescription>%2</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>"_s.arg(code)
                .arg(description);
        reply(socket, 500, xml.toUtf8());
    }

    static void reply(QTcpSocket *socket, int status, const QByteArray &body, bool inChunks = false)
    {
        QByteArray out = "HTTP/1.1 "_ba + QByteArray::number(status) + (status == 200 ? " OK"_ba : " Error"_ba) +
                         "\r\nContent-Type: text/xml\r\nConnection: close\r\n"_ba;
        if (inChunks) {
            out += "Transfer-Encoding: chunked\r\n\r\n"_ba;
            for (qsizetype at = 0; at < body.size(); at += 100) {
                const QByteArray chunk = body.mid(at, 100);
                out += QByteArray::number(chunk.size(), 16) + "\r\n"_ba + chunk + "\r\n"_ba;
            }
            out += "0\r\n\r\n"_ba;
        } else {
            out += "Content-Length: "_ba + QByteArray::number(body.size()) + "\r\n\r\n"_ba + body;
        }
        socket->write(out);
        socket->disconnectFromHost();
    }

    static QByteArray description()
    {
        return R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<friendlyName>Fake router</friendlyName>
<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
<controlURL>/ctl/L3F</controlURL></service></serviceList>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><controlURL>/ctl/IPConn</controlURL>
<eventSubURL>/evt/IPConn</eventSubURL><SCPDURL>/WANIPCn.xml</SCPDURL></service></serviceList>
</device></deviceList></device></deviceList></device></root>
)"_ba;
    }

    QUdpSocket m_ssdp;
    QTcpServer m_http;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

} // namespace selecta::core::testing
