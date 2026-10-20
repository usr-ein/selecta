// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Codec.h"
#include "slsk/Protocol.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>

#include <optional>
#include <variant>

// Peer-init, peer (`P`) and file (`F`) channel messages (references/soulseek-protocol.md §5–§7). Each struct follows
// the convention of SELECTA_SLSK_MESSAGE (Codec.h). Pure and reentrant.
//
// SharedFileListResponse, FileSearchResponse and FolderContentsResponse are zlib streams as a whole: encode()
// compresses and decode() inflates with an output cap (protocol::k*InflatedMax), so a hostile stream can't make a
// decoder allocate more than the cap.
namespace selecta::slsk {

// --- Peer init: the first message on every peer socket ---------------------------------------------------------------
namespace peerinit {

// "You asked the server for me with this token": the answer to an indirect connection request.
struct PierceFireWall
{
    SELECTA_SLSK_MESSAGE(PierceFireWall, PeerInitCode::PierceFireWall);
    quint32 token = 0;
};

// "I am `username`; this socket is of `type`": a direct connection.
struct PeerInit
{
    SELECTA_SLSK_MESSAGE(PeerInit, PeerInitCode::PeerInit);
    QString username;
    ConnectionType type = ConnectionType::Peer;
    quint32 token = 0; // always 0 today; read and ignored (aioslsk sends a 64-bit value)
};

} // namespace peerinit

// --- Peer messages ---------------------------------------------------------------------------------------------------
enum class TransferDirection : quint32 {
    Download = 0, // legacy: the downloader asks for a file (slskd, Seeker)
    Upload = 1,   // the uploader is ready to send a file
};

namespace peer {

// Asks for the recipient's whole share list.
struct GetShareFileList
{
    SELECTA_SLSK_MESSAGE(GetShareFileList, PeerCode::GetShareFileList);
};

struct SharedFileListResponse
{
    SELECTA_SLSK_MESSAGE(SharedFileListResponse, PeerCode::SharedFileListResponse);
    SharedFileList list;

    // decode() with a different inflate cap than protocol::kShareListInflatedMax.
    static std::optional<SharedFileListResponse> decode(QByteArrayView payload, qsizetype maxInflated);
};

// Search results from a peer, keyed by the token of our search.
struct FileSearchResponse
{
    SELECTA_SLSK_MESSAGE(FileSearchResponse, PeerCode::FileSearchResponse);
    QString username; // the responder, as it claims; the connection's own username is authoritative
    quint32 token = 0;
    QList<FileEntry> results; // full virtual paths
    bool slotFree = false;
    quint32 averageSpeed = 0; // bytes per second
    quint32 queueLength = 0;  // unreliable: several clients always send 0
    quint32 unknown = 0;
    QList<FileEntry> privateResults; // files the responder shares only with some users

    static std::optional<FileSearchResponse> decode(QByteArrayView payload, qsizetype maxInflated);

    // The username and token, read by inflating only the start of the stream: lets the receiver drop results for
    // searches it no longer collects without inflating them.
    struct Header
    {
        QString username;
        quint32 token = 0;
    };
    static std::optional<Header> peek(QByteArrayView payload);
};

struct UserInfoRequest
{
    SELECTA_SLSK_MESSAGE(UserInfoRequest, PeerCode::UserInfoRequest);
};

struct UserInfoResponse
{
    SELECTA_SLSK_MESSAGE(UserInfoResponse, PeerCode::UserInfoResponse);
    QString description;
    std::optional<QByteArray> picture; // image bytes, as sent
    quint32 totalUploads = 0;          // upload slots
    quint32 queueSize = 0;
    bool slotsFree = false;
    std::optional<quint32> uploadPermitted; // who may push files to the user; SoulseekQt omits it
};

struct FolderContentsRequest
{
    SELECTA_SLSK_MESSAGE(FolderContentsRequest, PeerCode::FolderContentsRequest);
    quint32 token = 0;
    RemotePath folder;
};

// One folder's files (Nicotine+ sends no subfolders; ask for each one). An unknown or forbidden folder gets an empty
// list.
struct FolderContentsResponse
{
    SELECTA_SLSK_MESSAGE(FolderContentsResponse, PeerCode::FolderContentsResponse);
    quint32 token = 0;
    RemotePath folder;
    QList<SharedDirectory> directories;

    static std::optional<FolderContentsResponse> decode(QByteArrayView payload, qsizetype maxInflated);
};

struct TransferRequest
{
    SELECTA_SLSK_MESSAGE(TransferRequest, PeerCode::TransferRequest);
    TransferDirection direction = TransferDirection::Upload;
    quint32 token = 0;
    RemotePath path;
    // Sent with direction Upload. 0 can mean "over 2 GiB" (SoulseekQt): keep the size from the search result then.
    std::optional<quint64> size;
};

struct TransferResponse
{
    SELECTA_SLSK_MESSAGE(TransferResponse, PeerCode::TransferResponse);
    quint32 token = 0;
    bool allowed = false;
    std::optional<quint64> size; // allowed, answering a legacy download request only
    QString reason;              // not allowed: see classifyRejection()
};

struct QueueUpload
{
    SELECTA_SLSK_MESSAGE(QueueUpload, PeerCode::QueueUpload);
    RemotePath path;
};

struct PlaceInQueueResponse
{
    SELECTA_SLSK_MESSAGE(PlaceInQueueResponse, PeerCode::PlaceInQueueResponse);
    RemotePath path;
    quint32 place = 0;
};

struct UploadFailed
{
    SELECTA_SLSK_MESSAGE(UploadFailed, PeerCode::UploadFailed);
    RemotePath path;
};

struct UploadDenied
{
    SELECTA_SLSK_MESSAGE(UploadDenied, PeerCode::UploadDenied);
    RemotePath path;
    QString reason;
};

struct PlaceInQueueRequest
{
    SELECTA_SLSK_MESSAGE(PlaceInQueueRequest, PeerCode::PlaceInQueueRequest);
    RemotePath path;
};

} // namespace peer

// --- File connections: raw, unframed ---------------------------------------------------------------------------------
namespace file {

inline constexpr qsizetype kTransferInitBytes = 4; // uploader → downloader: the transfer token
inline constexpr qsizetype kOffsetBytes = 8;       // downloader → uploader: bytes it already has

QByteArray encodeTransferInit(quint32 token);
std::optional<quint32> decodeTransferInit(QByteArrayView bytes); // exactly 4 bytes
QByteArray encodeOffset(quint64 offset);
std::optional<quint64> decodeOffset(QByteArrayView bytes); // exactly 8 bytes

} // namespace file

using PeerInitMessage = std::variant<peerinit::PierceFireWall, peerinit::PeerInit>;

using PeerMessage =
    std::variant<peer::GetShareFileList, peer::SharedFileListResponse, peer::FileSearchResponse, peer::UserInfoRequest,
                 peer::UserInfoResponse, peer::FolderContentsRequest, peer::FolderContentsResponse,
                 peer::TransferRequest, peer::TransferResponse, peer::QueueUpload, peer::PlaceInQueueResponse,
                 peer::UploadFailed, peer::UploadDenied, peer::PlaceInQueueRequest>;

// Decodes one payload by code (the zlib messages with their default caps). Recognised but undecoded codes are
// Ignored; others Unknown.
Decoded<PeerInitMessage> decodePeerInit(quint32 code, QByteArrayView payload);
Decoded<PeerMessage> decodePeer(quint32 code, QByteArrayView payload);
bool isKnownPeerCode(quint32 code);

// A username a peer may present (PeerInit, DistribBranchRoot): 1–256 bytes of UTF-8, printable, and not `server`.
bool isAcceptablePeerUsername(const QString &username);

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::TransferDirection)
Q_DECLARE_METATYPE(selecta::slsk::peer::FileSearchResponse)
Q_DECLARE_METATYPE(selecta::slsk::peer::UserInfoResponse)
Q_DECLARE_METATYPE(selecta::slsk::peer::FolderContentsResponse)
