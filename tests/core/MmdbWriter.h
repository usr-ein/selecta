// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test stand-ins for the country database (P-002): a minimal MaxMind DB writer (format 2.0, 24-bit records), so tests
// build small country databases of their own, and a loopback HTTP server that serves canned answers. Tests never
// download the real database.

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <zlib.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace selecta::core::testing {

using namespace Qt::StringLiterals;

class MmdbWriter
{
public:
    explicit MmdbWriter(int ipVersion = 6, QByteArray databaseType = "DBIP-Country-Lite")
        : m_ipVersion(ipVersion)
        , m_type(std::move(databaseType))
    {
        m_nodes.emplace_back();
    }

    // `network`/`prefix` in the country `iso` ("FR"); `iso` is written as given, so tests can write bad codes. An
    // IPv4 network goes under ::/96 in an IPv6 database, where libmaxminddb looks IPv4 addresses up.
    void add(const QString &network, int prefix, const QByteArray &iso) { addRecord(network, prefix, countryRecord(iso)); }
    // A network whose record has no country (a continent only).
    void addWithoutCountry(const QString &network, int prefix) { addRecord(network, prefix, continentRecord()); }
    // A network whose record points past the end of the data section: a damaged file.
    void addBroken(const QString &network, int prefix) { addRecord(network, prefix, std::nullopt); }

    QByteArray build() const
    {
        const auto nodeCount = static_cast<std::uint32_t>(m_nodes.size());
        QByteArray out;
        for (const Node &node : m_nodes) {
            for (const Record &record : node) {
                std::uint32_t value = nodeCount;
                if (record.kind == Kind::Node) {
                    value = record.value;
                } else if (record.kind == Kind::Data) {
                    value = nodeCount + 16 + record.value;
                } else if (record.kind == Kind::Broken) {
                    value = nodeCount + 16 + static_cast<std::uint32_t>(m_data.size()) + 1000;
                }
                out.append(static_cast<char>((value >> 16U) & 0xFFU));
                out.append(static_cast<char>((value >> 8U) & 0xFFU));
                out.append(static_cast<char>(value & 0xFFU));
            }
        }
        out.append(QByteArray(16, '\0'));
        out.append(m_data);
        out.append("\xAB\xCD\xEFMaxMind.com");
        QByteArray metadata = map(9);
        metadata += string("binary_format_major_version") + uint16(2);
        metadata += string("binary_format_minor_version") + uint16(0);
        metadata += string("build_epoch") + uint64(1'790'000'000);
        metadata += string("database_type") + string(m_type);
        metadata += string("description") + map(1) + string("en") + string("Selecta test countries");
        metadata += string("ip_version") + uint16(static_cast<std::uint16_t>(m_ipVersion));
        metadata += string("languages") + array(1) + string("en");
        metadata += string("node_count") + uint32(nodeCount);
        metadata += string("record_size") + uint16(24);
        out.append(metadata);
        return out;
    }

    static QByteArray gzip(const QByteArray &raw)
    {
        z_stream stream{};
        deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        QByteArray out(static_cast<qsizetype>(deflateBound(&stream, static_cast<uLong>(raw.size()))) + 32, '\0');
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(raw.constData()));
        stream.avail_in = static_cast<uInt>(raw.size());
        stream.next_out = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());
        deflate(&stream, Z_FINISH);
        out.truncate(static_cast<qsizetype>(stream.total_out));
        deflateEnd(&stream);
        return out;
    }

private:
    enum class Kind : std::uint8_t { Empty, Node, Data, Broken };
    struct Record
    {
        Kind kind = Kind::Empty;
        std::uint32_t value = 0;
    };
    using Node = std::array<Record, 2>;

    static QByteArray control(int type, std::uint32_t size)
    {
        QByteArray out;
        const auto typeBits = static_cast<unsigned>(type <= 7 ? type : 0) << 5U;
        if (size < 29) {
            out.append(static_cast<char>(typeBits | size));
        } else {
            out.append(static_cast<char>(typeBits | 29U));
        }
        if (type > 7) {
            out.append(static_cast<char>(type - 7));
        }
        if (size >= 29) {
            out.append(static_cast<char>(size - 29));
        }
        return out;
    }
    static QByteArray string(const QByteArray &text)
    {
        return control(2, static_cast<std::uint32_t>(text.size())) + text;
    }
    static QByteArray unsignedBytes(int type, std::uint64_t value, int width)
    {
        QByteArray bytes;
        for (int i = width - 1; i >= 0; --i) {
            bytes.append(static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU));
        }
        return control(type, static_cast<std::uint32_t>(width)) + bytes;
    }
    static QByteArray uint16(std::uint16_t value) { return unsignedBytes(5, value, 2); }
    static QByteArray uint32(std::uint32_t value) { return unsignedBytes(6, value, 4); }
    static QByteArray uint64(std::uint64_t value) { return unsignedBytes(9, value, 8); }
    static QByteArray map(std::uint32_t pairs) { return control(7, pairs); }
    static QByteArray array(std::uint32_t items) { return control(11, items); }

    static QByteArray countryRecord(const QByteArray &iso)
    {
        return map(1) + string("country") + map(1) + string("iso_code") + string(iso);
    }
    static QByteArray continentRecord()
    {
        return map(1) + string("continent") + map(1) + string("code") + string("EU");
    }

    void addRecord(const QString &network, int prefix, const std::optional<QByteArray> &data)
    {
        const QHostAddress address(network);
        std::vector<bool> bits;
        bool isV4 = false;
        const quint32 v4 = address.toIPv4Address(&isV4);
        if (isV4) {
            if (m_ipVersion == 6) {
                bits.assign(96, false);
            }
            for (int i = 31; i >= 0; --i) {
                bits.push_back(((v4 >> static_cast<unsigned>(i)) & 1U) != 0);
            }
            prefix += m_ipVersion == 6 ? 96 : 0;
        } else {
            const Q_IPV6ADDR v6 = address.toIPv6Address();
            for (const quint8 byte : v6.c) {
                for (int i = 7; i >= 0; --i) {
                    bits.push_back(((byte >> static_cast<unsigned>(i)) & 1U) != 0);
                }
            }
        }
        Record leaf;
        if (data) {
            leaf = {.kind = Kind::Data, .value = static_cast<std::uint32_t>(m_data.size())};
            m_data.append(*data);
        } else {
            leaf = {.kind = Kind::Broken, .value = 0};
        }
        std::size_t node = 0;
        for (int i = 0; i < prefix; ++i) {
            const auto side = static_cast<std::size_t>(bits.at(static_cast<std::size_t>(i)));
            if (i == prefix - 1) {
                m_nodes.at(node).at(side) = leaf;
                return;
            }
            if (m_nodes.at(node).at(side).kind != Kind::Node) {
                m_nodes.emplace_back();
                m_nodes.at(node).at(side) = {.kind = Kind::Node, .value = static_cast<std::uint32_t>(m_nodes.size() - 1)};
            }
            node = m_nodes.at(node).at(side).value;
        }
    }

    int m_ipVersion;
    QByteArray m_type;
    std::vector<Node> m_nodes;
    QByteArray m_data;
};

// An HTTP server on 127.0.0.1 that answers each path with a canned status and body, and counts the requests.
class FakeHttpServer
{
public:
    struct Answer
    {
        int status = 200;
        QByteArray body;
        bool hang = false;         // never answer
        qint64 claimedLength = -1; // a Content-Length other than the body's
    };

    FakeHttpServer()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { serve(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    void answer(const QString &path, Answer answer) { m_answers.insert(path, std::move(answer)); }
    QUrl url(const QString &path) const
    {
        return QUrl(u"http://127.0.0.1:%1%2"_s.arg(m_server.serverPort()).arg(path));
    }
    int requests(const QString &path) const { return m_requests.value(path); }
    int totalRequests() const
    {
        int total = 0;
        for (const int count : m_requests) {
            total += count;
        }
        return total;
    }

private:
    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        if (!buffer.contains("\r\n\r\n")) {
            return;
        }
        const QList<QByteArray> requestLine = buffer.left(buffer.indexOf("\r\n")).split(' ');
        m_buffers.remove(socket);
        const QString path = requestLine.size() >= 2 ? QString::fromLatin1(requestLine.at(1)) : QString();
        m_requests[path] += 1;
        const Answer answer =
            m_answers.value(path, Answer{.status = 404, .body = "not found", .hang = false, .claimedLength = -1});
        if (answer.hang) {
            return;
        }
        socket->write("HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\nContent-Length: " +
                      QByteArray::number(answer.claimedLength >= 0 ? answer.claimedLength : answer.body.size()) + "\r\nConnection: close\r\n\r\n" + answer.body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QString, Answer> m_answers;
    QHash<QString, int> m_requests;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

} // namespace selecta::core::testing
