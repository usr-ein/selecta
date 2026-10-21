// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "slsk/PeerMessages.h"
#include "slsk/PermitSource.h"
#include "slsk/Protocol.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>

class QTcpSocket;

namespace selecta::slsk {

class ServerSession;
class ShareProvider;
class UserInfoProvider;

namespace internal {
class PeerNetworkPrivate;
} // namespace internal

// Why a peer request or connection failed.
enum class PeerError {
    None,
    NotLoggedIn,      // no server session to broker the connection
    NotPermitted,     // the PermitSource refused the PeerLookup
    UserOffline,      // the server says the user isn't connected
    Unreachable,      // neither the direct nor the indirect connection worked
    SocketLimit,      // the global socket cap (setSocketLimit(), at most protocol::kSocketsMax) is reached
    Timeout,          // the peer didn't answer in time
    ConnectionClosed, // the connection closed before the answer arrived
    ProtocolError,    // the answer was malformed, oversize, or didn't inflate
    Cancelled,        // cancelled locally (closeAll(), shutdown)
};

const char *peerErrorName(PeerError error);

// Requests we make to a peer and wait for an answer to.
enum class PeerRequest {
    SharedFileList,
    UserInfo,
    FolderContents,
};

// Peer connections (references/soulseek-protocol.md §3, §5, §6): the listener, connection setup, `P` messages, and
// the hand-off of `F` sockets to TransferManager and `D` traffic to DistributedNode.
//
// Connections: a new outbound connection costs one PeerLookup permit from the PermitSource, then races the direct
// path (GetPeerAddress, dial, PeerInit) against the indirect one (ConnectToPeer, the peer dials back with
// PierceFireWall); the first to complete wins. Tokens come from a random start, increment, wrap and skip 0, and are
// matched against {token → user, type, deadline} (direct 10 s, indirect 20 s). When the server relays someone's
// ConnectToPeer we dial back with PierceFireWall, or send CantConnectToPeer if that fails; `F` only for an expected
// transfer, `D` only when DistributedNode wants it.
//
// Per user there is one `P` and one `D` connection we send on. If the peer opens a new one while one exists, the newest
// becomes the one we send on and the older one is only read from until it closes or idles out, so no message in flight
// is lost. Every connection closes after 60 s idle (10 s if it never identifies itself); a `P` connection a peer opened
// only to deliver search results is closed once they're read; the one we open to deliver ours is closed once it's
// sent. A global cap (setSocketLimit(), at most protocol::kSocketsMax) bounds sockets, `F` ones included.
//
// Answers to peers' requests come from ShareProvider and UserInfoProvider (unbudgeted). Large responses are accepted
// only while we wait for one from that user (share lists up to 256 MiB, user info up to 16 MiB); an unsolicited one
// closes the connection. Share lists are inflated and parsed on a worker thread.
//
// Threading: lives on the engine thread and is used only from it. Ownership: a QObject; owns its sockets until it hands
// an `F` socket over. Not owned: the ServerSession and PermitSource (they must outlive it) and the providers.
// Failures are reported per request (requestFailed) or per send (the `done` callbacks); nothing fails silently. A call
// that starts work never reports from inside itself: a failure known at once arrives on a later event-loop turn.
// closeAll() is the exception: it reports every cancellation before it returns.
class PeerNetwork : public QObject
{
    Q_OBJECT

public:
    struct Timeouts
    {
        std::chrono::milliseconds connect = protocol::kPeerConnectTimeout;
        std::chrono::milliseconds indirect = protocol::kPeerIndirectTimeout;
        std::chrono::milliseconds idle = protocol::kPeerIdleTimeout;
        std::chrono::milliseconds init = protocol::kPeerInitTimeout;
        std::chrono::milliseconds response = protocol::kPeerResponseTimeout;
    };

    // Called once per send with PeerError::None when the frame was written to a connection, or why it couldn't be.
    using SendCallback = std::function<void(PeerError error)>;

    PeerNetwork(ServerSession &server, PermitSource &permits, QObject *parent = nullptr);
    ~PeerNetwork() override;
    PeerNetwork(const PeerNetwork &) = delete;
    PeerNetwork &operator=(const PeerNetwork &) = delete;

    void setTimeouts(const Timeouts &timeouts);
    void setSocketLimit(int sockets); // clamped to 1…protocol::kSocketsMax
    void setShareProvider(ShareProvider *provider);
    void setUserInfoProvider(UserInfoProvider *provider);

    // --- listener ----------------------------------------------------------------------------------------------------

    // Listens on all IPv4 interfaces (the protocol is IPv4 only). Port 0 picks a free port. False, with `error` set,
    // when the port can't be bound. The caller reports the port to the server (ServerSession::setListenPort()).
    bool listen(quint16 port, QString *error = nullptr);
    // Listens on one address only (tests use loopback).
    bool listen(quint16 port, const QHostAddress &address, QString *error = nullptr);
    void stopListening();
    bool isListening() const;
    quint16 listenPort() const;

    // --- our requests: each consumes its permit (a connection, if one is needed, asks the PermitSource) --------------

    // Browse a user's shares: sharedFileListReceived() or requestFailed(). False (permit dropped) when the permit is
    // for another operation or `username` is empty.
    bool requestSharedFileList(const QString &username, governor::Permit browse);
    bool requestUserInfo(const QString &username, governor::Permit userInfo);
    // The files in one remote folder; returns the request token (0 on refusal). folderContentsReceived() or
    // requestFailed() with that token.
    quint32 requestFolderContents(const QString &username, const RemotePath &folder, governor::Permit folderContents);

    // --- search results ----------------------------------------------------------------------------------------------

    // Results are accepted only for open tokens: others are dropped after reading just their header.
    void openSearch(quint32 token);
    void closeSearch(quint32 token);
    bool isSearchOpen(quint32 token) const;
    // Delivers our answer to someone's search (unbudgeted; a new connection asks the PermitSource with purpose
    // SearchResponse). The connection is closed once the answer is written, unless something else uses it.
    void sendSearchResponse(const QString &username, const peer::FileSearchResponse &response, SendCallback done = {});

    // --- generic `P` sends (TransferManager) -------------------------------------------------------------------------

    // Sends a framed peer message, opening a connection if needed.
    void sendPeerMessage(const QString &username, const QByteArray &bytes, ConnectPurpose purpose,
                         SendCallback done = {});
    // True while a `P` connection to `username` is established or being set up.
    bool hasPeerConnection(const QString &username) const;

    // --- `F` connections (TransferManager) ---------------------------------------------------------------------------

    // Opens a file connection to `username` (uploads; purpose Transfer). Returns an id; fileConnectionReady() or
    // fileConnectionFailed() follows with it, never before this returns.
    quint64 openFileConnection(const QString &username);
    // Gives up on an openFileConnection() id that hasn't been reported yet. Nothing more is heard of it.
    void cancelFileConnection(quint64 id);
    // Whether to dial back when `username` asks for an `F` connection through the server (only for transfers we
    // expect). Without a filter, such requests are refused.
    void setFileConnectionFilter(std::function<bool(const QString &username)> filter);

    // --- `D` connections (DistributedNode) ---------------------------------------------------------------------------

    // Connects to a parent candidate at a known address: direct dial raced against ConnectToPeer (purpose
    // Distributed). distributedConnected() or distributedConnectFailed() follows.
    void openDistributedConnection(const QString &username, const QHostAddress &address, quint16 port);
    void sendDistributedMessage(const QString &username, const QByteArray &bytes);
    void closeDistributedConnection(const QString &username);
    // Whether to dial back when `username` asks for a `D` connection through the server. Without a filter, refused.
    void setDistributedConnectionFilter(std::function<bool(const QString &username)> filter);

    // --- housekeeping ------------------------------------------------------------------------------------------------

    // Closes every connection and fails every pending request and send with PeerError::Cancelled. The listener stays.
    void closeAll();
    int socketCount() const;
    int connectionCount(ConnectionType type) const;

Q_SIGNALS:
    // Answers to our requests. `address` is the remote end of the connection the results came on (null when unknown),
    // for the peer's country flag (P-002); nothing else asks the server for it.
    void searchResponseReceived(const QString &username, const selecta::slsk::peer::FileSearchResponse &response,
                                const QHostAddress &address);
    void sharedFileListReceived(const QString &username, const selecta::slsk::SharedFileList &list);
    void sharedFileListProgress(const QString &username, qint64 received, qint64 total);
    void userInfoReceived(const QString &username, const selecta::slsk::peer::UserInfoResponse &info);
    void folderContentsReceived(const QString &username, const selecta::slsk::peer::FolderContentsResponse &contents);
    // `token` is the folder-contents token, 0 for the other requests.
    void requestFailed(const QString &username, selecta::slsk::PeerRequest request, quint32 token,
                       selecta::slsk::PeerError error);

    // Peers' requests we answered (for the activity log; the answers came from the providers).
    void sharedFileListRequested(const QString &username);
    void userInfoRequested(const QString &username);
    void folderContentsRequested(const QString &username, const selecta::slsk::RemotePath &folder);

    // Transfer negotiation on `P` (TransferManager).
    void transferRequestReceived(const QString &username, const selecta::slsk::peer::TransferRequest &request);
    void transferResponseReceived(const QString &username, const selecta::slsk::peer::TransferResponse &response);
    void queueUploadReceived(const QString &username, const selecta::slsk::peer::QueueUpload &request);
    void placeInQueueRequestReceived(const QString &username, const selecta::slsk::peer::PlaceInQueueRequest &request);
    void placeInQueueResponseReceived(const QString &username,
                                      const selecta::slsk::peer::PlaceInQueueResponse &response);
    void uploadFailedReceived(const QString &username, const selecta::slsk::peer::UploadFailed &message);
    void uploadDeniedReceived(const QString &username, const selecta::slsk::peer::UploadDenied &message);

    // An `F` connection is up. `id` is openFileConnection()'s id, or 0 when the peer opened it (or we dialled back).
    // Ownership of `socket` passes to the receiver, which must take it in a direct connection; `pending` holds bytes
    // already read from it.
    void fileConnectionReady(quint64 id, const QString &username, QTcpSocket *socket, const QByteArray &pending);
    void fileConnectionFailed(quint64 id, const QString &username, selecta::slsk::PeerError error);

    // `D` traffic (DistributedNode). `outbound` is false when the peer connected to us (a would-be child).
    void distributedConnected(const QString &username, bool outbound);
    void distributedConnectFailed(const QString &username, selecta::slsk::PeerError error);
    void distributedMessageReceived(const QString &username, quint32 code, const QByteArray &payload);
    void distributedDisconnected(const QString &username);

    // A peer opened a connection to our listening port and identified itself (PeerInit, or PierceFireWall answering
    // our ConnectToPeer): proof that the port is reachable from the outside (NET-3's reachability check).
    void reachedByPeer(const QString &username);

private:
    friend class internal::PeerNetworkPrivate;
    std::unique_ptr<internal::PeerNetworkPrivate> d;
};

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::PeerError)
Q_DECLARE_METATYPE(selecta::slsk::PeerRequest)
