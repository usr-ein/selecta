// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fixtures/FakeServer.h"
#include "slsk/DistributedMessages.h"
#include "slsk/Frame.h"
#include "slsk/PeerMessages.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>

#include <functional>
#include <map>
#include <optional>

class QTcpServer;
class QTcpSocket;

namespace selecta::slsk::testing {

// A scriptable remote peer for tests, written from the protocol description rather than with slsk's own session code
// (it shares only the codec), so the two check each other. It listens on loopback, is known to a FakeServer as a
// virtual user, answers requests from configurable data, and can open connections, send anything, upload and
// download files, and act as a distributed parent or child.
//
// Threading: lives on the test's thread. Ownership: owns its sockets.
class FakePeer : public QObject, public FakeServer::VirtualUser
{
    Q_OBJECT

public:
    // What a connection carries.
    enum class Mode {
        AwaitingInit, // inbound, nothing identified yet
        Peer,
        Distributed,
        FileUpload,   // we send a file: token sent, waiting for the offset, then streaming
        FileDownload, // we receive a file: waiting for the token, then the bytes
    };

    // A file the peer can upload: content, and how to misbehave while sending it.
    struct Upload
    {
        QByteArray content;
        qint64 stopAfter = -1;       // close the F connection after this many bytes (a killed transfer)
        bool reportZeroSize = false; // SoulseekQt sends size 0 for files over 2 GiB
    };

    // How the peer answers a QueueUpload.
    enum class QueueBehaviour {
        StartUpload, // send TransferRequest for the file right away (if it has it), else deny
        Hold,        // keep it queued; the test calls startUpload()
        Deny,        // UploadDenied with denyReason
        Ignore,      // say nothing
    };

    FakePeer(QString username, FakeServer *server, QObject *parent = nullptr);
    ~FakePeer() override;
    FakePeer(const FakePeer &) = delete;
    FakePeer &operator=(const FakePeer &) = delete;

    QString username() const { return m_username; }
    // Listens on 127.0.0.1 (any port) and registers with the server. With `reachable` false, the server hands out a
    // closed port instead, so only the indirect path can reach this peer.
    bool start(bool reachable = true);
    quint16 port() const;

    // --- behaviour ---------------------------------------------------------------------------------------------------
    bool answerIndirect = true;     // dial back when the server relays a ConnectToPeer
    bool reportCantConnect = false; // when not answering, say so through the server (CantConnectToPeer)
    bool refuseInbound = false;     // close every inbound connection at once
    bool ignoreRequests = false;    // never answer browse, user info or folder contents
    SharedFileList shares;
    peer::UserInfoResponse userInfo;
    QueueBehaviour queueBehaviour = QueueBehaviour::StartUpload;
    QString denyReason = QStringLiteral("File not shared.");
    std::optional<quint32> placeInQueue; // answer PlaceInQueueRequest with this, or not at all
    QHash<QByteArray, Upload> files;     // raw remote path → what to send
    // As a downloader (when we upload to it): decline our TransferRequest with this reason (empty: allow), and the
    // resume offset to ask for.
    QString declineUploads;
    quint64 resumeOffset = 0;
    bool ignoreTransferRequests = false; // never answer an uploader's TransferRequest
    qint64 closeDownloadAfter = -1;      // as a downloader, hang up after this many bytes
    // As an uploader: open the F connection and send the token, then never send a byte.
    bool stallUploads = false;
    // As an uploader: allow-and-forget, i.e. never open the F connection after the downloader allows it.
    bool neverConnectFile = false;
    QStringList searchHits; // answer a search with the shares' files whose path contains a hit word
    // Called for every peer message, after the built-in answers.
    std::function<void(quint64 connection, const QString &from, const PeerMessage &message)> onMessage;

    // --- actions -----------------------------------------------------------------------------------------------------
    // Opens a connection to `user` at `address`:`port` and identifies with PeerInit. Returns a connection id. An F
    // connection opened this way carries our upload.
    quint64 connectTo(const QString &user, const QHostAddress &address, quint16 port, ConnectionType type);
    // Opens an F connection as the downloader of an upload `user` wants to send us (Soulseek.NET opens it itself when
    // the uploader's connection doesn't arrive): we wait for their token, then send our offset.
    quint64 connectAsDownloader(const QString &user, const QHostAddress &address, quint16 port);
    // Asks the server to have `user` connect to us (they answer with PierceFireWall `token`).
    bool requestIndirect(const QString &user, ConnectionType type, quint32 token);
    bool send(quint64 connection, const QByteArray &bytes);
    // Sends on the newest identified P connection with `user`.
    bool sendTo(const QString &user, const QByteArray &bytes);
    void close(quint64 connection);
    void closeAll();
    // Sends TransferRequest(upload) for a held queue entry.
    bool startUpload(const QString &user, const RemotePath &path);

    // --- distributed -------------------------------------------------------------------------------------------------
    // As a parent on connection `connection`: level, root, searches.
    bool sendDistrib(quint64 connection, const QByteArray &bytes);

    // --- observations ------------------------------------------------------------------------------------------------
    struct Received
    {
        quint64 connection = 0;
        QString from;
        PeerMessage message;
    };
    QList<Received> received;
    QList<std::pair<quint64, std::pair<quint32, QByteArray>>> distribReceived; // connection → (code, payload)
    QList<quint32> pierceTokens;          // PierceFireWall tokens received on inbound connections
    QStringList peerInitsFrom;            // usernames that identified with PeerInit on inbound connections
    QList<QByteArray> downloadedFiles;    // complete files received over F
    QList<RemotePath> uploadFailuresSent; // UploadFailed echoes it sent after a downloader cut a transfer
    int indirectRequests = 0;

    template <typename M>
    QList<M> receivedOf() const
    {
        QList<M> matching;
        for (const Received &r : received) {
            if (const M *m = std::get_if<M>(&r.message)) {
                matching.append(*m);
            }
        }
        return matching;
    }

    int connectionCount() const { return static_cast<int>(m_connections.size()); }
    int identifiedCount(ConnectionType type) const;
    bool isOpen(quint64 connection) const { return m_connections.contains(connection); }

    // FakeServer::VirtualUser
    void connectToPeerRequested(const fromserver::ConnectToPeerRequest &request) override;
    void searchRequested(const fromserver::SearchRequest &request) override;
    void cantConnectToPeer(quint32 token) override;

Q_SIGNALS:
    void identified(quint64 connection, const QString &peer, selecta::slsk::ConnectionType type);
    void messageReceived(quint64 connection, const QString &from, const selecta::slsk::PeerMessage &message);
    void closed(quint64 connection);

private:
    struct Connection
    {
        QTcpSocket *socket = nullptr;
        bool outbound = false;
        Mode mode = Mode::AwaitingInit;
        QString peer;
        FrameReader reader{CodeWidth::Byte, protocol::kPeerInitFrameMax};
        QByteArray raw; // F connections
        // F upload state
        quint32 transferToken = 0;
        RemotePath path;
        QByteArray uploadContent;
        qint64 stopAfter = -1;
        bool offsetReceived = false;
        // F download state
        quint64 expectedSize = 0;
        bool tokenReceived = false;
    };

    struct PendingUpload
    {
        QString user;
        RemotePath path;
        quint32 token = 0;
    };

    quint64 add(QTcpSocket *socket, bool outbound);
    void onReadyRead(quint64 id);
    void onClosed(quint64 id);
    void identify(quint64 id, Mode mode, const QString &peer);
    void handleInit(quint64 id, const Frame &frame);
    void handlePeerMessage(quint64 id, const PeerMessage &message);
    struct Responder;
    friend struct Responder;
    void handleFileUpload(quint64 id);
    void handleFileDownload(quint64 id);
    void openFileConnection(const QString &user, quint32 token);
    Connection *find(quint64 id);

    QString m_username;
    FakeServer *m_server;
    QTcpServer *m_listener = nullptr;
    std::map<quint64, Connection> m_connections;
    QHash<quint32, std::pair<QString, ConnectionType>> m_indirectTokens; // our requestIndirect() tokens
    QHash<quint32, PendingUpload> m_uploads;                             // TransferRequest tokens we sent
    QHash<quint32, std::pair<QString, quint64>> m_downloads;             // tokens we allowed → (user, size)
    QList<PendingUpload> m_held;                                         // queue entries held for startUpload()
    QHash<QString, QHostAddress> m_addresses;                            // where peers connect from
    quint64 m_nextId = 1;
    quint32 m_nextToken = 5000;
};

} // namespace selecta::slsk::testing
