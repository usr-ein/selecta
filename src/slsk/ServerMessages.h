// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Codec.h"
#include "slsk/Protocol.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <variant>

// Server channel messages (references/soulseek-protocol.md §4, §12.1, §12.7), in both directions: `toserver` is what
// a client sends, `fromserver` what the server sends. Selecta itself encodes `toserver` and decodes `fromserver`; the
// test FakeServer does the reverse, so every message encodes and decodes both ways. Each struct follows the
// convention of SELECTA_SLSK_MESSAGE (Codec.h). Pure and reentrant.
namespace selecta::slsk {

// A member of a chat room, as JoinRoom and UserJoinedRoom describe it.
struct RoomMember
{
    QString username;
    UserStatus status = UserStatus::Offline;
    UserStats stats;
    quint32 slotsFull = 0; // 1 when the user's upload slots are full
    QString countryCode;   // upper case; often empty

    friend bool operator==(const RoomMember &, const RoomMember &) = default;
};

// A room and how many users are in it.
struct RoomListEntry
{
    QString name;
    quint32 userCount = 0;

    friend bool operator==(const RoomListEntry &, const RoomListEntry &) = default;
};

// A user the server offers as our distributed parent.
struct ParentCandidate
{
    QString username;
    quint32 ip = 0; // see ipv4Address()
    quint32 port = 0;

    friend bool operator==(const ParentCandidate &, const ParentCandidate &) = default;
};

// --- Client → server -------------------------------------------------------------------------------------------------
namespace toserver {

struct Login
{
    SELECTA_SLSK_MESSAGE(Login, ServerCode::Login);
    QString username;
    QString password;
    quint32 majorVersion = protocol::kMajorVersion;
    QByteArray hash; // md5Hex(username + password)
    quint32 minorVersion = protocol::kMinorVersion;

    // A login with Selecta's version numbers and the hash filled in.
    static Login make(const QString &username, const QString &password);
};

struct SetWaitPort
{
    SELECTA_SLSK_MESSAGE(SetWaitPort, ServerCode::SetWaitPort);
    quint32 port = 0;
    // SoulseekQt's obfuscated second port. Selecta never sends it (protocol §3.4) but decodes it.
    std::optional<quint32> obfuscationType;
    std::optional<quint32> obfuscatedPort;
};

struct GetPeerAddress
{
    SELECTA_SLSK_MESSAGE(GetPeerAddress, ServerCode::GetPeerAddress);
    QString username;
};

struct WatchUser
{
    SELECTA_SLSK_MESSAGE(WatchUser, ServerCode::WatchUser);
    QString username;
};

struct UnwatchUser
{
    SELECTA_SLSK_MESSAGE(UnwatchUser, ServerCode::UnwatchUser);
    QString username;
};

struct GetUserStatus
{
    SELECTA_SLSK_MESSAGE(GetUserStatus, ServerCode::GetUserStatus);
    QString username;
};

struct SayChatroom
{
    SELECTA_SLSK_MESSAGE(SayChatroom, ServerCode::SayChatroom);
    QString room;
    QString message;
};

struct JoinRoom
{
    SELECTA_SLSK_MESSAGE(JoinRoom, ServerCode::JoinRoom);
    QString room;
    quint32 isPrivate = 0; // Selecta always sends 0 (D32: no private rooms)
};

struct LeaveRoom
{
    SELECTA_SLSK_MESSAGE(LeaveRoom, ServerCode::LeaveRoom);
    QString room;
};

struct ConnectToPeer
{
    SELECTA_SLSK_MESSAGE(ConnectToPeer, ServerCode::ConnectToPeer);
    quint32 token = 0;
    QString username;
    ConnectionType type = ConnectionType::Peer;
};

struct MessageUser
{
    SELECTA_SLSK_MESSAGE(MessageUser, ServerCode::MessageUser);
    QString username;
    QString message;
};

struct MessageAcked
{
    SELECTA_SLSK_MESSAGE(MessageAcked, ServerCode::MessageAcked);
    quint32 id = 0;
};

struct FileSearch
{
    SELECTA_SLSK_MESSAGE(FileSearch, ServerCode::FileSearch);
    quint32 token = 0;
    QString query;
};

struct SetStatus
{
    SELECTA_SLSK_MESSAGE(SetStatus, ServerCode::SetStatus);
    qint32 status = static_cast<qint32>(UserStatus::Online); // 1 away, 2 online
};

struct ServerPing
{
    SELECTA_SLSK_MESSAGE(ServerPing, ServerCode::ServerPing);
};

struct SharedFoldersFiles
{
    SELECTA_SLSK_MESSAGE(SharedFoldersFiles, ServerCode::SharedFoldersFiles);
    quint32 folders = 0;
    quint32 files = 0;
};

struct GetUserStats
{
    SELECTA_SLSK_MESSAGE(GetUserStats, ServerCode::GetUserStats);
    QString username;
};

struct UserSearch
{
    SELECTA_SLSK_MESSAGE(UserSearch, ServerCode::UserSearch);
    QString username;
    quint32 token = 0;
    QString query;
};

struct RoomList
{
    SELECTA_SLSK_MESSAGE(RoomList, ServerCode::RoomList);
};

struct HaveNoParent
{
    SELECTA_SLSK_MESSAGE(HaveNoParent, ServerCode::HaveNoParent);
    bool noParent = true;
};

struct CheckPrivileges
{
    SELECTA_SLSK_MESSAGE(CheckPrivileges, ServerCode::CheckPrivileges);
};

struct AcceptChildren
{
    SELECTA_SLSK_MESSAGE(AcceptChildren, ServerCode::AcceptChildren);
    bool accept = false;
};

struct WishlistSearch
{
    SELECTA_SLSK_MESSAGE(WishlistSearch, ServerCode::WishlistSearch);
    quint32 token = 0;
    QString query;
};

struct SendUploadSpeed
{
    SELECTA_SLSK_MESSAGE(SendUploadSpeed, ServerCode::SendUploadSpeed);
    quint32 bytesPerSecond = 0;
};

struct BranchLevel
{
    SELECTA_SLSK_MESSAGE(BranchLevel, ServerCode::BranchLevel);
    quint32 level = 0;
};

struct BranchRoot
{
    SELECTA_SLSK_MESSAGE(BranchRoot, ServerCode::BranchRoot);
    QString root;
};

struct EnableRoomInvitations
{
    SELECTA_SLSK_MESSAGE(EnableRoomInvitations, ServerCode::EnableRoomInvitations);
    bool enable = false;
};

struct CantConnectToPeer
{
    SELECTA_SLSK_MESSAGE(CantConnectToPeer, ServerCode::CantConnectToPeer);
    quint32 token = 0;
    QString username;
};

} // namespace toserver

// --- Server → client -------------------------------------------------------------------------------------------------
namespace fromserver {

// The reply to Login. On success the server greets us; on failure it names a reason.
struct LoginReply
{
    SELECTA_SLSK_MESSAGE(LoginReply, ServerCode::Login);
    bool success = false;
    // Success:
    QString greeting;        // message of the day
    quint32 ownIp = 0;       // our address as the server sees it
    QByteArray passwordHash; // md5Hex(password)
    bool isSupporter = false;
    // Failure: INVALIDUSERNAME (with a detail), EMPTYPASSWORD, INVALIDPASS, INVALIDVERSION, SVRFULL, SVRPRIVATE, …
    QString reason;
    QString detail;
};

struct PeerAddress
{
    SELECTA_SLSK_MESSAGE(PeerAddress, ServerCode::GetPeerAddress);
    QString username;
    quint32 ip = 0; // 0.0.0.0 with port 0: the user is offline
    quint32 port = 0;
    std::optional<quint32> obfuscationType;
    std::optional<quint16> obfuscatedPort;
};

struct WatchUserReply
{
    SELECTA_SLSK_MESSAGE(WatchUserReply, ServerCode::WatchUser);
    QString username;
    bool exists = false;
    // Only when the user exists:
    UserStatus status = UserStatus::Offline;
    UserStats stats;
    QString countryCode; // only when away or online
};

struct UserStatusUpdate
{
    SELECTA_SLSK_MESSAGE(UserStatusUpdate, ServerCode::GetUserStatus);
    QString username;
    UserStatus status = UserStatus::Offline;
    bool privileged = false;
};

struct RoomMessage
{
    SELECTA_SLSK_MESSAGE(RoomMessage, ServerCode::SayChatroom);
    QString room;
    QString username;
    QString message;
};

// The reply to JoinRoom: everyone in the room. Private rooms add an owner and operators.
struct RoomJoined
{
    SELECTA_SLSK_MESSAGE(RoomJoined, ServerCode::JoinRoom);
    QString room;
    QList<RoomMember> members; // at most protocol::kRoomMembersMax
    std::optional<QString> owner;
    QStringList operators;
};

struct RoomLeft
{
    SELECTA_SLSK_MESSAGE(RoomLeft, ServerCode::LeaveRoom);
    QString room;
};

struct RoomUserJoined
{
    SELECTA_SLSK_MESSAGE(RoomUserJoined, ServerCode::UserJoinedRoom);
    QString room;
    RoomMember member;
};

struct RoomUserLeft
{
    SELECTA_SLSK_MESSAGE(RoomUserLeft, ServerCode::UserLeftRoom);
    QString room;
    QString username;
};

// Another user asks us to connect to them (the indirect path, protocol §3.2).
struct ConnectToPeerRequest
{
    SELECTA_SLSK_MESSAGE(ConnectToPeerRequest, ServerCode::ConnectToPeer);
    QString username;
    ConnectionType type = ConnectionType::Peer;
    quint32 ip = 0;
    quint32 port = 0;
    quint32 token = 0; // goes back in PierceFireWall
    bool privileged = false;
    std::optional<quint32> obfuscationType;
    std::optional<quint32> obfuscatedPort;
};

struct PrivateMessage
{
    SELECTA_SLSK_MESSAGE(PrivateMessage, ServerCode::MessageUser);
    quint32 id = 0;
    quint32 timestamp = 0; // seconds since the Unix epoch
    QString username;
    QString message;
    bool isNew = true; // false: re-delivered after we were offline
};

// Someone searches our shares outside the distributed network (a user or room search; Soulfind sends every search
// this way).
struct SearchRequest
{
    SELECTA_SLSK_MESSAGE(SearchRequest, ServerCode::FileSearch);
    QString username;
    quint32 token = 0;
    QString query;
};

struct UserStatsUpdate
{
    SELECTA_SLSK_MESSAGE(UserStatsUpdate, ServerCode::GetUserStats);
    QString username;
    UserStats stats;
};

struct Relogged
{
    SELECTA_SLSK_MESSAGE(Relogged, ServerCode::Relogged);
};

struct RoomList
{
    SELECTA_SLSK_MESSAGE(RoomList, ServerCode::RoomList);
    QList<RoomListEntry> publicRooms; // at most protocol::kRoomListMax, like each list below
    QList<RoomListEntry> ownedPrivateRooms;
    QList<RoomListEntry> memberPrivateRooms;
    QStringList operatedPrivateRooms;
};

struct AdminMessage
{
    SELECTA_SLSK_MESSAGE(AdminMessage, ServerCode::AdminMessage);
    QString message;
};

struct PrivilegedUsers
{
    SELECTA_SLSK_MESSAGE(PrivilegedUsers, ServerCode::PrivilegedUsers);
    QStringList users;
};

struct ParentMinSpeed
{
    SELECTA_SLSK_MESSAGE(ParentMinSpeed, ServerCode::ParentMinSpeed);
    quint32 speed = 0;
};

struct ParentSpeedRatio
{
    SELECTA_SLSK_MESSAGE(ParentSpeedRatio, ServerCode::ParentSpeedRatio);
    quint32 ratio = 0;
};

struct PrivilegesLeft
{
    SELECTA_SLSK_MESSAGE(PrivilegesLeft, ServerCode::CheckPrivileges);
    quint32 seconds = 0;
};

// A distributed message the server feeds us directly: we are a branch root. `payload` is the distributed message
// body without length or code; only DistribSearch (code 3) is meaningful.
struct EmbeddedMessage
{
    SELECTA_SLSK_MESSAGE(EmbeddedMessage, ServerCode::EmbeddedMessage);
    quint8 distribCode = 0;
    QByteArray payload;
};

struct PossibleParents
{
    SELECTA_SLSK_MESSAGE(PossibleParents, ServerCode::PossibleParents);
    QList<ParentCandidate> candidates; // at most protocol::kPossibleParentsMax
};

struct WishlistInterval
{
    SELECTA_SLSK_MESSAGE(WishlistInterval, ServerCode::WishlistInterval);
    quint32 seconds = 0;
};

struct ResetDistributed
{
    SELECTA_SLSK_MESSAGE(ResetDistributed, ServerCode::ResetDistributed);
};

struct RoomInvitationsEnabled
{
    SELECTA_SLSK_MESSAGE(RoomInvitationsEnabled, ServerCode::EnableRoomInvitations);
    bool enabled = false;
};

struct ExcludedSearchPhrases
{
    SELECTA_SLSK_MESSAGE(ExcludedSearchPhrases, ServerCode::ExcludedSearchPhrases);
    QStringList phrases;
};

struct CantConnectToPeer
{
    SELECTA_SLSK_MESSAGE(CantConnectToPeer, ServerCode::CantConnectToPeer);
    quint32 token = 0;
};

struct CantCreateRoom
{
    SELECTA_SLSK_MESSAGE(CantCreateRoom, ServerCode::CantCreateRoom);
    QString room;
};

} // namespace fromserver

using ToServerMessage =
    std::variant<toserver::Login, toserver::SetWaitPort, toserver::GetPeerAddress, toserver::WatchUser,
                 toserver::UnwatchUser, toserver::GetUserStatus, toserver::SayChatroom, toserver::JoinRoom,
                 toserver::LeaveRoom, toserver::ConnectToPeer, toserver::MessageUser, toserver::MessageAcked,
                 toserver::FileSearch, toserver::SetStatus, toserver::ServerPing, toserver::SharedFoldersFiles,
                 toserver::GetUserStats, toserver::UserSearch, toserver::RoomList, toserver::HaveNoParent,
                 toserver::CheckPrivileges, toserver::AcceptChildren, toserver::WishlistSearch,
                 toserver::SendUploadSpeed, toserver::BranchLevel, toserver::BranchRoot,
                 toserver::EnableRoomInvitations, toserver::CantConnectToPeer>;

using FromServerMessage =
    std::variant<fromserver::LoginReply, fromserver::PeerAddress, fromserver::WatchUserReply,
                 fromserver::UserStatusUpdate, fromserver::RoomMessage, fromserver::RoomJoined, fromserver::RoomLeft,
                 fromserver::RoomUserJoined, fromserver::RoomUserLeft, fromserver::ConnectToPeerRequest,
                 fromserver::PrivateMessage, fromserver::SearchRequest, fromserver::UserStatsUpdate,
                 fromserver::Relogged, fromserver::RoomList, fromserver::AdminMessage, fromserver::PrivilegedUsers,
                 fromserver::ParentMinSpeed, fromserver::ParentSpeedRatio, fromserver::PrivilegesLeft,
                 fromserver::EmbeddedMessage, fromserver::PossibleParents, fromserver::WishlistInterval,
                 fromserver::ResetDistributed, fromserver::RoomInvitationsEnabled, fromserver::ExcludedSearchPhrases,
                 fromserver::CantConnectToPeer, fromserver::CantCreateRoom>;

// Decodes one server-channel payload by code. Recognised codes outside the decoded set are Ignored; codes not in
// ServerCode are Unknown.
Decoded<ToServerMessage> decodeToServer(quint32 code, QByteArrayView payload);
Decoded<FromServerMessage> decodeFromServer(quint32 code, QByteArrayView payload);
// True for codes in ServerCode.
bool isKnownServerCode(quint32 code);

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::RoomMember)
Q_DECLARE_METATYPE(selecta::slsk::RoomListEntry)
Q_DECLARE_METATYPE(selecta::slsk::ParentCandidate)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::LoginReply)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::PeerAddress)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::WatchUserReply)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::UserStatusUpdate)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::RoomMessage)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::RoomJoined)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::RoomUserJoined)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::RoomUserLeft)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::ConnectToPeerRequest)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::PrivateMessage)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::SearchRequest)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::UserStatsUpdate)
Q_DECLARE_METATYPE(selecta::slsk::fromserver::RoomList)
