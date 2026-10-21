// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Limits.h"

#include <QStringView>
#include <QtTypes>

#include <chrono>

// Wire constants of the Soulseek protocol (references/soulseek-protocol.md §2, §4–§8, §12). Pure data; usable from
// any thread.
namespace selecta::slsk {

// --- Message codes ---------------------------------------------------------------------------------------------------

// Server channel: `uint32 length` · `uint32 code` · payload. Codes Selecta neither sends nor decodes are skipped by
// length; the ones listed here are recognised (decoded, or deliberately dropped).
enum class ServerCode : quint32 {
    Login = 1,
    SetWaitPort = 2,
    GetPeerAddress = 3,
    WatchUser = 5,
    UnwatchUser = 6,
    GetUserStatus = 7,
    IgnoreUser = 11,   // obsolete, dropped
    UnignoreUser = 12, // obsolete, dropped
    SayChatroom = 13,
    JoinRoom = 14,
    LeaveRoom = 15,
    UserJoinedRoom = 16,
    UserLeftRoom = 17,
    ConnectToPeer = 18,
    MessageUser = 22,
    MessageAcked = 23,
    FileSearchRoom = 25, // obsolete, dropped
    FileSearch = 26,
    SetStatus = 28,
    ServerPing = 32,
    SendConnectToken = 33,  // obsolete, dropped
    SendDownloadSpeed = 34, // obsolete, never sent
    SharedFoldersFiles = 35,
    GetUserStats = 36,
    UploadSlotsFull = 40, // obsolete, dropped
    Relogged = 41,
    UserSearch = 42,
    SimilarRecommendations = 50, // interests and recommendations (50–60, 110–112, 117–118): dropped
    AddThingILike = 51,
    RemoveThingILike = 52,
    Recommendations = 54,
    MyRecommendations = 55,
    GlobalRecommendations = 56,
    UserInterests = 57,
    AdminCommand = 58,
    PlaceInLineRequest = 59,
    PlaceInLineResponse = 60,
    RoomAdded = 62,
    RoomRemoved = 63,
    RoomList = 64,
    ExactFileSearch = 65,
    AdminMessage = 66,
    GlobalUserList = 67,
    TunneledMessage = 68,
    PrivilegedUsers = 69,
    HaveNoParent = 71,
    ParentIp = 73,
    ParentMinSpeed = 83,
    ParentSpeedRatio = 84,
    ParentInactivityTimeout = 86,
    SearchInactivityTimeout = 87,
    MinParentsInCache = 88,
    DistribPingInterval = 90,
    AddToPrivileged = 91,
    CheckPrivileges = 92,
    EmbeddedMessage = 93,
    AcceptChildren = 100,
    PossibleParents = 102,
    WishlistSearch = 103,
    WishlistInterval = 104,
    SimilarUsers = 110,
    ItemRecommendations = 111,
    ItemSimilarUsers = 112,
    RoomTickers = 113,
    RoomTickerAdded = 114,
    RoomTickerRemoved = 115,
    SetRoomTicker = 116,
    AddThingIHate = 117,
    RemoveThingIHate = 118,
    RoomSearch = 120,
    SendUploadSpeed = 121,
    UserPrivileged = 122,
    GivePrivileges = 123,
    NotifyPrivileges = 124,
    AckNotifyPrivileges = 125,
    BranchLevel = 126,
    BranchRoot = 127,
    ChildDepth = 129,
    ResetDistributed = 130,
    RoomMembers = 133, // private rooms (133–140, 142–148): dropped (D32 scope)
    AddRoomMember = 134,
    RemoveRoomMember = 135,
    CancelRoomMembership = 136,
    CancelRoomOwnership = 137,
    RoomSomething = 138,
    RoomMembershipGranted = 139,
    RoomMembershipRevoked = 140,
    EnableRoomInvitations = 141,
    ChangePassword = 142,
    AddRoomOperator = 143,
    RemoveRoomOperator = 144,
    RoomOperatorshipGranted = 145,
    RoomOperatorshipRevoked = 146,
    RoomOperators = 148,
    MessageUsers = 149,
    JoinGlobalRoom = 150,
    LeaveGlobalRoom = 151,
    GlobalRoomMessage = 152,
    RelatedSearch = 153,
    ExcludedSearchPhrases = 160,
    CantConnectToPeer = 1001,
    CantCreateRoom = 1003,
};

// Peer-init channel (the first message on every peer socket): `uint32 length` · `uint8 code` · payload.
enum class PeerInitCode : quint8 {
    PierceFireWall = 0,
    PeerInit = 1,
};

// Peer channel (`P` connections): `uint32 length` · `uint32 code` · payload.
enum class PeerCode : quint32 {
    PrivateMessage = 1, // obsolete, dropped
    GetShareFileList = 4,
    SharedFileListResponse = 5,
    FileSearchRequest = 8, // obsolete, dropped
    FileSearchResponse = 9,
    RoomInvitation = 10,          // obsolete, dropped
    CancelledQueuedTransfer = 14, // obsolete, dropped
    UserInfoRequest = 15,
    UserInfoResponse = 16,
    SendConnectToken = 33,  // obsolete, dropped
    MoveDownloadToTop = 34, // obsolete, dropped
    FolderContentsRequest = 36,
    FolderContentsResponse = 37,
    TransferRequest = 40,
    TransferResponse = 41,
    PlaceholdUpload = 42, // obsolete, dropped
    QueueUpload = 43,
    PlaceInQueueResponse = 44,
    UploadFailed = 46,
    ExactFileSearchRequest = 47,    // obsolete, dropped
    QueuedDownloads = 48,           // obsolete, dropped
    IndirectFileSearchRequest = 49, // obsolete, dropped
    UploadDenied = 50,
    PlaceInQueueRequest = 51,
    UploadQueueNotification = 52, // deprecated, dropped
};

// Distributed channel (`D` connections): `uint32 length` · `uint8 code` · payload.
enum class DistribCode : quint8 {
    Ping = 0, // deprecated, dropped
    Search = 3,
    BranchLevel = 4,
    BranchRoot = 5,
    ChildDepth = 7, // obsolete, dropped
    EmbeddedMessage = 93,
};

namespace protocol {

using namespace std::chrono_literals;

// --- Identity (NET-9, protocol §12.2) --------------------------------------------------------------------------------
// Major 177 is the community registry's number for experimental clients. Never 157 (official), 160 (Nicotine+),
// 169 (seeleseek), 170 (Soulseek.NET), 175 (aioslsk) or 176 (soulseek-rs). A reserved number is requested before
// the first live session (M9). The minor version is bumped when Selecta gains a network capability.
inline constexpr quint32 kMajorVersion = 177;
inline constexpr quint32 kMinorVersion = 1;
static_assert(kMajorVersion == 177, "Selecta identifies as an experimental client until a number is reserved");
static_assert(kMinorVersion != 0, "the server rejects minor version 0");

inline constexpr quint16 kDefaultListenPort = 2234;
// The pseudo-user that sends server notices as private messages. Peers may never claim this name.
inline constexpr QStringView kServerUsername = u"server";
// Identifier carried by every DistribSearch (ASCII '1'); messages with another value are rejected.
inline constexpr quint32 kDistribSearchIdentifier = 49;

// --- Inbound size caps (NFR-4, protocol §12.4) -----------------------------------------------------------------------
// A frame whose length field exceeds its cap closes the connection before any of its payload is buffered.
inline constexpr qsizetype kKiB = 1024;
inline constexpr qsizetype kMiB = 1024 * kKiB;
inline constexpr qsizetype kGiB = 1024 * kMiB;
inline constexpr qsizetype kServerFrameMax = 16 * kMiB;
inline constexpr qsizetype kPeerInitFrameMax = 16 * kKiB;
inline constexpr qsizetype kPeerFrameMax = 1 * kMiB;           // every peer message not listed below
inline constexpr qsizetype kPeerLargeFrameMax = 16 * kMiB;     // search results, folder contents, unknown codes
inline constexpr qsizetype kPeerLargeInflatedMax = 128 * kMiB; // the same, after inflating
inline constexpr qsizetype kShareListFrameMax = 256 * kMiB;    // only while we wait for one from that user
inline constexpr qsizetype kShareListInflatedMax = 1 * kGiB;
inline constexpr qsizetype kUserInfoFrameMax = 16 * kMiB; // only while we wait for one from that user
inline constexpr qsizetype kDistribFrameMax = 16 * kKiB;
// Usernames read from peers (PeerInit, DistribBranchRoot, search results): the server allows 30 characters; the
// extra room follows Nicotine+.
inline constexpr qsizetype kUsernameMaxBytes = 256;
// Every length-prefixed string read from the network (Reader::string()): names, paths and free text alike. Real ones
// stay far below it (a Windows long path is 32,767 UTF-16 units; chat lines and profile texts are a few KiB), so a
// longer one is hostile and makes its message malformed. Without it, a name could take a whole frame (up to 1 GiB
// once a share list inflates) and be copied once per file downstream. The user-info picture is binary, not a string,
// and is bounded by its frame instead (kUserInfoFrameMax).
inline constexpr qsizetype kStringMaxBytes = 64 * kKiB;
// Decode-time sanity caps for counts that the server controls (protocol §12.7).
inline constexpr qsizetype kRoomMembersMax = 50'000;
inline constexpr qsizetype kRoomListMax = 100'000;
inline constexpr qsizetype kPossibleParentsMax = 1'000;

// --- Outbound text (etiquette §4.6, protocol §12.7) ------------------------------------------------------------------
inline constexpr qsizetype kChatLineMaxBytes = 2000; // UTF-8 bytes of one room line or private message
inline constexpr qsizetype kRoomNameMaxChars = 24;

// --- Timers (protocol §11, §12.4; governor/Limits.h) -----------------------------------------------------------------
inline constexpr std::chrono::milliseconds kServerConnectTimeout = 30s;
inline constexpr std::chrono::milliseconds kLoginReplyTimeout = governor::limits::kLoginReplyTimeout;
inline constexpr std::chrono::milliseconds kServerPingInterval = 300s;
inline constexpr std::chrono::seconds kKeepAliveIdle = 60s;
inline constexpr std::chrono::seconds kKeepAliveInterval = 10s;
inline constexpr int kKeepAliveProbes = 6;
inline constexpr std::chrono::milliseconds kPeerConnectTimeout = governor::limits::kPeerConnectTimeout;
inline constexpr std::chrono::milliseconds kPeerIndirectTimeout = governor::limits::kPeerIndirectTimeout;
inline constexpr std::chrono::milliseconds kPeerIdleTimeout = governor::limits::kPeerIdleTimeout;
inline constexpr std::chrono::milliseconds kPeerInitTimeout = 10s;      // an accepted socket must identify itself
inline constexpr std::chrono::milliseconds kPeerResponseTimeout = 60s;  // a request with no reply and no traffic
inline constexpr std::chrono::milliseconds kFileRequestedTimeout = 60s; // allowed download → F connection
inline constexpr std::chrono::milliseconds kFileHandshakeTimeout = 20s; // F connection → token / offset
inline constexpr std::chrono::milliseconds kFileStallTimeout = 60s;     // no file bytes in either direction
inline constexpr std::chrono::milliseconds kUploadStartTimeout = governor::limits::kUploadStartTimeout;
inline constexpr std::chrono::milliseconds kUploadLinger = 30s; // all bytes sent → wait for the downloader to close
inline constexpr std::chrono::milliseconds kProgressInterval = 250ms; // progress coalesced to ~4 signals a second
inline constexpr int kSocketsMax = governor::limits::kSocketsMax;
inline constexpr int kDistribChildrenMax = 10; // Nicotine+: "limit to 10 for now due to socket limit concerns"
inline constexpr int kParentCandidatesMax = 10;

} // namespace protocol
} // namespace selecta::slsk
