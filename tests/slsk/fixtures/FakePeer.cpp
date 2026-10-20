// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures/FakePeer.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <ranges>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::slsk::testing {
namespace {

// A port nothing listens on: bound once, then released.
quint16 closedPort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 1;
    }
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

} // namespace

namespace {

// The mode of a connection we opened, or one a peer opened to us with PeerInit (`inbound`): an F connection we open
// carries our upload; one opened to us carries a download.
FakePeer::Mode modeFor(ConnectionType type, bool weAreUploader)
{
    switch (type) {
    case ConnectionType::Peer:
        return FakePeer::Mode::Peer;
    case ConnectionType::Distributed:
        return FakePeer::Mode::Distributed;
    case ConnectionType::File:
        return weAreUploader ? FakePeer::Mode::FileUpload : FakePeer::Mode::FileDownload;
    }
    return FakePeer::Mode::Peer;
}

ConnectionType typeFor(FakePeer::Mode mode)
{
    switch (mode) {
    case FakePeer::Mode::Peer:
    case FakePeer::Mode::AwaitingInit:
        return ConnectionType::Peer;
    case FakePeer::Mode::Distributed:
        return ConnectionType::Distributed;
    case FakePeer::Mode::FileUpload:
    case FakePeer::Mode::FileDownload:
        return ConnectionType::File;
    }
    return ConnectionType::Peer;
}

} // namespace

FakePeer::FakePeer(QString username, FakeServer *server, QObject *parent)
    : QObject(parent)
    , m_username(std::move(username))
    , m_server(server)
    , m_listener(new QTcpServer(this))
{
    connect(m_listener, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = m_listener->nextPendingConnection()) {
            if (refuseInbound) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            const quint64 id = add(socket, false);
            if (socket->bytesAvailable() > 0) {
                onReadyRead(id);
            }
        }
    });
}

FakePeer::~FakePeer()
{
    if (m_server != nullptr) {
        m_server->removeVirtualUser(m_username);
    }
    for (auto &[id, connection] : m_connections) {
        connection.socket->disconnect(this);
        connection.socket->abort();
    }
}

bool FakePeer::start(bool reachable)
{
    if (!m_listener->listen(QHostAddress::LocalHost, 0)) {
        return false;
    }
    if (m_server != nullptr) {
        m_server->addVirtualUser(m_username, QHostAddress::LocalHost, reachable ? port() : closedPort(), this);
    }
    return true;
}

quint16 FakePeer::port() const
{
    return m_listener->serverPort();
}

int FakePeer::identifiedCount(ConnectionType type) const
{
    return static_cast<int>(std::ranges::count_if(m_connections, [type](const auto &entry) {
        return entry.second.mode != Mode::AwaitingInit && typeFor(entry.second.mode) == type;
    }));
}

FakePeer::Connection *FakePeer::find(quint64 id)
{
    const auto it = m_connections.find(id);
    return it == m_connections.end() ? nullptr : &it->second;
}

quint64 FakePeer::add(QTcpSocket *socket, bool outbound)
{
    const quint64 id = m_nextId++;
    socket->setParent(this);
    Connection c;
    c.socket = socket;
    c.outbound = outbound;
    m_connections.emplace(id, std::move(c));
    connect(socket, &QTcpSocket::readyRead, this, [this, id] { onReadyRead(id); });
    connect(socket, &QTcpSocket::disconnected, this, [this, id] { onClosed(id); });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, id] { onClosed(id); });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, id] {
        if (const Connection *c = find(id); c != nullptr && c->mode == Mode::FileUpload) {
            handleFileUpload(id);
        }
    });
    return id;
}

void FakePeer::onClosed(quint64 id)
{
    const auto it = m_connections.find(id);
    if (it == m_connections.end()) {
        return;
    }
    // Like real uploaders: a transfer cut short is reported on P, so the downloader can re-queue (or ignore it).
    if (it->second.mode == Mode::FileUpload && it->second.offsetReceived && !it->second.path.isEmpty() &&
        it->second.socket->bytesToWrite() > 0) {
        const QString user = it->second.peer;
        const RemotePath path = it->second.path;
        uploadFailuresSent.append(path);
        QTimer::singleShot(0, this, [this, user, path] { sendTo(user, frame(peer::UploadFailed{path})); });
    }
    QTcpSocket *socket = it->second.socket;
    m_connections.erase(it);
    socket->disconnect(this);
    socket->deleteLater();
    Q_EMIT closed(id);
}

void FakePeer::close(quint64 connection)
{
    if (const Connection *c = find(connection)) {
        c->socket->abort();
        onClosed(connection);
    }
}

void FakePeer::closeAll()
{
    QList<quint64> ids;
    for (const auto &[id, c] : m_connections) {
        ids.append(id);
    }
    for (const quint64 id : std::as_const(ids)) {
        close(id);
    }
}

void FakePeer::identify(quint64 id, Mode mode, const QString &peer)
{
    Connection &c = m_connections.at(id);
    const QByteArray rest = c.reader.takeBuffered();
    c.mode = mode;
    c.peer = peer;
    if (mode == Mode::Peer) {
        c.reader = FrameReader(CodeWidth::Word, protocol::kShareListFrameMax);
        c.reader.append(rest);
    } else if (mode == Mode::Distributed) {
        c.reader = FrameReader(CodeWidth::Byte, protocol::kDistribFrameMax);
        c.reader.append(rest);
    } else {
        c.raw = rest;
    }
    m_addresses.insert(peer, c.socket->peerAddress());
    Q_EMIT identified(id, peer, typeFor(mode));
}

quint64 FakePeer::connectTo(const QString &user, const QHostAddress &address, quint16 port, ConnectionType type)
{
    auto *socket = new QTcpSocket(this);
    const quint64 id = add(socket, true);
    socket->connectToHost(address, port);
    // Written now, sent once connected: PeerInit is always the first message.
    socket->write(frame(peerinit::PeerInit{m_username, type, 0}));
    identify(id, modeFor(type, true), user);
    return id;
}

quint64 FakePeer::connectAsDownloader(const QString &user, const QHostAddress &address, quint16 port)
{
    auto *socket = new QTcpSocket(this);
    const quint64 id = add(socket, true);
    socket->connectToHost(address, port);
    socket->write(frame(peerinit::PeerInit{m_username, ConnectionType::File, 0}));
    identify(id, Mode::FileDownload, user);
    return id;
}

bool FakePeer::requestIndirect(const QString &user, ConnectionType type, quint32 token)
{
    m_indirectTokens.insert(token, {user, type});
    return m_server != nullptr && m_server->relayConnectToPeer(m_username, user, type, token);
}

bool FakePeer::send(quint64 connection, const QByteArray &bytes)
{
    const Connection *c = find(connection);
    return c != nullptr && c->socket->write(bytes) == bytes.size();
}

bool FakePeer::sendTo(const QString &user, const QByteArray &bytes)
{
    for (const auto &[id, connection] : std::views::reverse(m_connections)) {
        if (connection.mode == Mode::Peer && connection.peer == user) {
            return send(id, bytes);
        }
    }
    return false;
}

bool FakePeer::sendDistrib(quint64 connection, const QByteArray &bytes)
{
    return send(connection, bytes);
}

void FakePeer::onReadyRead(quint64 id)
{
    Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    const QByteArray data = c->socket->readAll();
    if (c->mode == Mode::FileUpload || c->mode == Mode::FileDownload) {
        c->raw.append(data);
        if (c->mode == Mode::FileUpload) {
            handleFileUpload(id);
        } else {
            handleFileDownload(id);
        }
        return;
    }
    c->reader.append(data);
    while ((c = find(id)) != nullptr) {
        if (c->mode == Mode::FileUpload || c->mode == Mode::FileDownload) {
            onReadyRead(id); // the init frame switched us to raw mode; handle what followed it
            return;
        }
        const std::optional<Frame> f = c->reader.next();
        if (!f) {
            if (c->reader.hasError()) {
                close(id);
            }
            return;
        }
        if (c->mode == Mode::AwaitingInit) {
            handleInit(id, *f);
        } else if (c->mode == Mode::Peer) {
            const Decoded<PeerMessage> decoded = decodePeer(f->code, f->payload);
            if (decoded.ok()) {
                handlePeerMessage(id, *decoded.message);
            }
        } else if (c->mode == Mode::Distributed) {
            distribReceived.append({id, {f->code, f->payload}});
        }
    }
}

void FakePeer::handleInit(quint64 id, const Frame &f)
{
    const Decoded<PeerInitMessage> decoded = decodePeerInit(f.code, f.payload);
    if (!decoded.ok()) {
        close(id);
        return;
    }
    if (const auto *init = std::get_if<peerinit::PeerInit>(&*decoded.message)) {
        peerInitsFrom.append(init->username);
        // An inbound F connection comes from the uploader: we are the downloader.
        identify(id, modeFor(init->type, false), init->username);
        return;
    }
    const quint32 token = std::get<peerinit::PierceFireWall>(*decoded.message).token;
    pierceTokens.append(token);
    const auto it = m_indirectTokens.constFind(token);
    if (it == m_indirectTokens.constEnd()) {
        close(id);
        return;
    }
    const auto [user, type] = it.value();
    m_indirectTokens.erase(it);
    if (type == ConnectionType::File) {
        // We asked them to dial back for our upload: we're the uploader, so we send the transfer token.
        for (auto upload = m_uploads.constBegin(); upload != m_uploads.constEnd(); ++upload) {
            if (upload->user == user) {
                identify(id, Mode::FileUpload, user);
                Connection &c = m_connections.at(id);
                c.transferToken = upload.key();
                c.path = upload->path;
                c.uploadContent = files.value(upload->path.raw).content;
                c.stopAfter = files.value(upload->path.raw).stopAfter;
                c.socket->write(file::encodeTransferInit(upload.key()));
                return;
            }
        }
        close(id);
        return;
    }
    identify(id, type == ConnectionType::Peer ? Mode::Peer : Mode::Distributed, user);
}

// Built-in answers, one per message type.
struct FakePeer::Responder
{
    FakePeer &peer;
    quint64 id;
    const QString &from;

    void operator()(peer::GetShareFileList /*m*/) const
    {
        if (!peer.ignoreRequests) {
            peer.send(id, frame(peer::SharedFileListResponse{peer.shares}));
        }
    }

    void operator()(peer::UserInfoRequest /*m*/) const
    {
        if (!peer.ignoreRequests) {
            peer.send(id, frame(peer.userInfo));
        }
    }

    void operator()(const peer::FolderContentsRequest &m) const
    {
        if (peer.ignoreRequests) {
            return;
        }
        peer::FolderContentsResponse response{m.token, m.folder, {}};
        for (const SharedDirectory &directory : std::as_const(peer.shares.directories)) {
            if (directory.path == m.folder) {
                response.directories.append(directory);
            }
        }
        peer.send(id, frame(response));
    }

    void operator()(const peer::QueueUpload &m) const
    {
        switch (peer.queueBehaviour) {
        case QueueBehaviour::StartUpload:
            if (peer.files.contains(m.path.raw)) {
                peer.m_held.append(PendingUpload{from, m.path, 0});
                peer.startUpload(from, m.path);
            } else {
                peer.send(id, frame(peer::UploadDenied{m.path, u"File not shared."_s}));
            }
            return;
        case QueueBehaviour::Hold:
            peer.m_held.append(PendingUpload{from, m.path, 0});
            return;
        case QueueBehaviour::Deny:
            peer.send(id, frame(peer::UploadDenied{m.path, peer.denyReason}));
            return;
        case QueueBehaviour::Ignore:
            return;
        }
    }

    void operator()(const peer::PlaceInQueueRequest &m) const
    {
        if (peer.placeInQueue) {
            peer.send(id, frame(peer::PlaceInQueueResponse{m.path, *peer.placeInQueue}));
        }
    }

    void operator()(const peer::TransferResponse &m) const
    {
        const auto upload = peer.m_uploads.constFind(m.token);
        if (upload == peer.m_uploads.constEnd()) {
            return;
        }
        if (m.allowed) {
            if (!peer.neverConnectFile) {
                peer.openFileConnection(upload->user, m.token);
            }
        } else {
            peer.m_uploads.erase(upload);
        }
    }

    void operator()(const peer::TransferRequest &m) const
    {
        if (m.direction == TransferDirection::Upload && !peer.ignoreTransferRequests) {
            if (!peer.declineUploads.isEmpty()) {
                peer.send(id, frame(peer::TransferResponse{m.token, false, std::nullopt, peer.declineUploads}));
                return;
            }
            // They're ready to upload to us: allow it and wait for their F connection.
            peer.m_downloads.insert(m.token, {from, m.size.value_or(0)});
            peer.send(id, frame(peer::TransferResponse{m.token, true, std::nullopt, {}}));
        }
    }

    // Everything else is only recorded.
    template <typename M>
    void operator()(const M & /*m*/) const
    {}
};

void FakePeer::handlePeerMessage(quint64 id, const PeerMessage &message)
{
    const QString from = m_connections.at(id).peer;
    received.append(Received{id, from, message});
    std::visit(Responder{*this, id, from}, message);
    if (onMessage) {
        onMessage(id, from, message);
    }
    Q_EMIT messageReceived(id, from, message);
}

bool FakePeer::startUpload(const QString &user, const RemotePath &path)
{
    const auto held =
        std::ranges::find_if(m_held, [&](const PendingUpload &p) { return p.user == user && p.path == path; });
    if (held == m_held.end()) {
        return false;
    }
    m_held.erase(held);
    const quint32 token = m_nextToken++;
    m_uploads.insert(token, PendingUpload{user, path, token});
    const Upload upload = files.value(path.raw);
    const quint64 size = upload.reportZeroSize ? 0 : static_cast<quint64>(upload.content.size());
    return sendTo(user, frame(peer::TransferRequest{TransferDirection::Upload, token, path, size}));
}

void FakePeer::openFileConnection(const QString &user, quint32 token)
{
    const FakeServer::Client *client = m_server != nullptr ? m_server->client(user) : nullptr;
    const PendingUpload upload = m_uploads.value(token);
    if (client != nullptr && client->listenPort != 0) {
        const quint64 id =
            connectTo(user, QHostAddress::LocalHost, static_cast<quint16>(client->listenPort), ConnectionType::File);
        Connection &c = m_connections.at(id);
        c.transferToken = token;
        c.path = upload.path;
        c.uploadContent = files.value(upload.path.raw).content;
        c.stopAfter = files.value(upload.path.raw).stopAfter;
        c.socket->write(file::encodeTransferInit(token));
        return;
    }
    // They don't listen: ask them to dial back.
    requestIndirect(user, ConnectionType::File, m_nextToken++);
}

void FakePeer::handleFileUpload(quint64 id)
{
    Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    if (!c->offsetReceived) {
        if (c->raw.size() < file::kOffsetBytes) {
            return;
        }
        const quint64 offset = file::decodeOffset(c->raw.first(file::kOffsetBytes)).value_or(0);
        c->raw.remove(0, file::kOffsetBytes);
        c->offsetReceived = true;
        if (stallUploads) {
            return; // keep the connection open and silent
        }
        const auto start = static_cast<qsizetype>(std::min<quint64>(offset, c->uploadContent.size()));
        QByteArray body = c->uploadContent.mid(start);
        if (c->stopAfter >= 0 && c->stopAfter < body.size()) {
            body.truncate(c->stopAfter);
            // A killed transfer happens once; the retry gets the whole rest.
            for (auto &upload : files) {
                if (upload.content == c->uploadContent) {
                    upload.stopAfter = -1;
                }
            }
            c->socket->write(body);
            c->socket->flush();
            c->socket->disconnectFromHost();
            return;
        }
        c->socket->write(body);
        // The downloader closes once it has everything; we only wait.
    }
}

void FakePeer::handleFileDownload(quint64 id)
{
    Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    if (!c->tokenReceived) {
        if (c->raw.size() < file::kTransferInitBytes) {
            return;
        }
        const quint32 token = file::decodeTransferInit(c->raw.first(file::kTransferInitBytes)).value_or(0);
        c->raw.remove(0, file::kTransferInitBytes);
        const auto download = m_downloads.constFind(token);
        if (download == m_downloads.constEnd()) {
            close(id);
            return;
        }
        c->tokenReceived = true;
        c->expectedSize = download->second > resumeOffset ? download->second - resumeOffset : 0;
        c->socket->write(file::encodeOffset(resumeOffset));
    }
    if (closeDownloadAfter >= 0 && c->raw.size() >= closeDownloadAfter) {
        close(id); // a downloader that gives up half-way
        return;
    }
    if (std::cmp_greater_equal(c->raw.size(), c->expectedSize)) {
        downloadedFiles.append(c->raw.left(static_cast<qsizetype>(c->expectedSize)));
        close(id); // the downloader closes
    }
}

void FakePeer::connectToPeerRequested(const fromserver::ConnectToPeerRequest &request)
{
    ++indirectRequests;
    if (!answerIndirect) {
        if (reportCantConnect && m_server != nullptr) {
            m_server->relayCantConnectToPeer(request.token, request.username);
        }
        return;
    }
    auto *socket = new QTcpSocket(this);
    const quint64 id = add(socket, true);
    socket->connectToHost(ipv4Address(request.ip), static_cast<quint16>(request.port));
    socket->write(frame(peerinit::PierceFireWall{request.token}));
    // We answer their request: as a downloader on F (they upload), as a parent on D.
    identify(id, modeFor(request.type, false), request.username);
}

void FakePeer::cantConnectToPeer(quint32 /*token*/) {}

void FakePeer::searchRequested(const fromserver::SearchRequest &request)
{
    if (searchHits.isEmpty() || request.username == m_username) {
        return;
    }
    peer::FileSearchResponse response;
    response.username = m_username;
    response.token = request.token;
    response.slotFree = true;
    response.averageSpeed = 100'000;
    for (const SharedDirectory &directory : std::as_const(shares.directories)) {
        for (const FileEntry &file : directory.files) {
            const QString path = directory.path.display() + u'\\' + file.name.display();
            const bool hit = std::ranges::any_of(searchHits, [&](const QString &word) {
                return request.query.contains(word, Qt::CaseInsensitive) && path.contains(word, Qt::CaseInsensitive);
            });
            if (hit) {
                FileEntry result = file;
                result.name = RemotePath::fromText(path);
                response.results.append(result);
            }
        }
    }
    if (response.results.isEmpty()) {
        return;
    }
    const FakeServer::Client *client = m_server != nullptr ? m_server->client(request.username) : nullptr;
    if (client == nullptr || client->listenPort == 0) {
        return;
    }
    const quint64 id = connectTo(request.username, QHostAddress::LocalHost, static_cast<quint16>(client->listenPort),
                                 ConnectionType::Peer);
    send(id, frame(response));
}

} // namespace selecta::slsk::testing
