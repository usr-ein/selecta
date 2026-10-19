// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/ServerMessages.h"

#include "slsk/internal/CodecHelpers.h"

#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::slsk {

using internal::encodeWith;
using internal::parse;
using internal::readOptional;
using internal::readStringList;
using internal::readText;
using internal::writeStringList;

namespace {

void writeStats(Writer &w, const UserStats &stats)
{
    w.u32(stats.averageSpeed);
    w.u32(stats.uploadCount);
    w.u32(stats.unknown);
    w.u32(stats.files);
    w.u32(stats.folders);
}

UserStats readStats(Reader &r)
{
    UserStats stats;
    stats.averageSpeed = r.u32();
    stats.uploadCount = r.u32();
    stats.unknown = r.u32();
    stats.files = r.u32();
    stats.folders = r.u32();
    return stats;
}

bool isPresent(UserStatus status)
{
    return status == UserStatus::Away || status == UserStatus::Online;
}

// One of the parallel lists of RoomList: names, then a count list of the same length.
QList<RoomListEntry> readRoomEntries(Reader &r)
{
    const qsizetype n = r.count(4);
    if (n > protocol::kRoomListMax) {
        r.fail();
        return {};
    }
    QList<RoomListEntry> entries;
    entries.reserve(n);
    for (qsizetype i = 0; i < n && r.ok(); ++i) {
        entries.append(RoomListEntry{readText(r), 0});
    }
    if (std::cmp_not_equal(r.u32(), n)) {
        r.fail(); // the counts must pair up with the names (protocol §12.7)
        return {};
    }
    if (n * 4 > r.remaining()) {
        r.fail();
        return {};
    }
    for (RoomListEntry &entry : entries) {
        entry.userCount = r.u32();
    }
    return entries;
}

void writeRoomEntries(Writer &w, const QList<RoomListEntry> &entries)
{
    w.u32(static_cast<quint32>(entries.size()));
    for (const RoomListEntry &entry : entries) {
        w.string(entry.name);
    }
    w.u32(static_cast<quint32>(entries.size()));
    for (const RoomListEntry &entry : entries) {
        w.u32(entry.userCount);
    }
}

} // namespace

// --- toserver --------------------------------------------------------------------------------------------------------
namespace toserver {

Login Login::make(const QString &username, const QString &password)
{
    Login login;
    login.username = username;
    login.password = password;
    login.hash = md5Hex(QString(username + password).toUtf8());
    return login;
}

QByteArray Login::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.string(password);
        w.u32(majorVersion);
        w.string(QByteArrayView(hash));
        w.u32(minorVersion);
    });
}

std::optional<Login> Login::decode(QByteArrayView payload)
{
    return parse<Login>(payload, [](Reader &r, Login &m) {
        m.username = readText(r);
        m.password = readText(r);
        m.majorVersion = r.u32();
        m.hash = r.string();
        m.minorVersion = r.u32();
    });
}

QByteArray SetWaitPort::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(port);
        if (obfuscationType || obfuscatedPort) {
            w.u32(obfuscationType.value_or(0));
            w.u32(obfuscatedPort.value_or(0));
        }
    });
}

std::optional<SetWaitPort> SetWaitPort::decode(QByteArrayView payload)
{
    return parse<SetWaitPort>(payload, [](Reader &r, SetWaitPort &m) {
        m.port = r.u32();
        if (r.remaining() >= 8) {
            m.obfuscationType = r.u32();
            m.obfuscatedPort = r.u32();
        }
    });
}

QByteArray GetPeerAddress::encode() const
{
    return internal::encodeSingle<&GetPeerAddress::username>(*this);
}

std::optional<GetPeerAddress> GetPeerAddress::decode(QByteArrayView payload)
{
    return internal::decodeSingle<GetPeerAddress, &GetPeerAddress::username>(payload);
}

QByteArray WatchUser::encode() const
{
    return internal::encodeSingle<&WatchUser::username>(*this);
}

std::optional<WatchUser> WatchUser::decode(QByteArrayView payload)
{
    return internal::decodeSingle<WatchUser, &WatchUser::username>(payload);
}

QByteArray UnwatchUser::encode() const
{
    return internal::encodeSingle<&UnwatchUser::username>(*this);
}

std::optional<UnwatchUser> UnwatchUser::decode(QByteArrayView payload)
{
    return internal::decodeSingle<UnwatchUser, &UnwatchUser::username>(payload);
}

QByteArray GetUserStatus::encode() const
{
    return internal::encodeSingle<&GetUserStatus::username>(*this);
}

std::optional<GetUserStatus> GetUserStatus::decode(QByteArrayView payload)
{
    return internal::decodeSingle<GetUserStatus, &GetUserStatus::username>(payload);
}

QByteArray GetUserStats::encode() const
{
    return internal::encodeSingle<&GetUserStats::username>(*this);
}

std::optional<GetUserStats> GetUserStats::decode(QByteArrayView payload)
{
    return internal::decodeSingle<GetUserStats, &GetUserStats::username>(payload);
}

QByteArray SayChatroom::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(room);
        w.string(message);
    });
}

std::optional<SayChatroom> SayChatroom::decode(QByteArrayView payload)
{
    return parse<SayChatroom>(payload, [](Reader &r, SayChatroom &m) {
        m.room = readText(r);
        m.message = readText(r);
    });
}

QByteArray JoinRoom::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(room);
        w.u32(isPrivate);
    });
}

std::optional<JoinRoom> JoinRoom::decode(QByteArrayView payload)
{
    return parse<JoinRoom>(payload, [](Reader &r, JoinRoom &m) {
        m.room = readText(r);
        if (r.remaining() >= 4) {
            m.isPrivate = r.u32();
        }
    });
}

QByteArray LeaveRoom::encode() const
{
    return encodeWith([this](Writer &w) { w.string(room); });
}

std::optional<LeaveRoom> LeaveRoom::decode(QByteArrayView payload)
{
    return parse<LeaveRoom>(payload, [](Reader &r, LeaveRoom &m) { m.room = readText(r); });
}

QByteArray ConnectToPeer::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(token);
        w.string(username);
        w.string(QByteArrayView(connectionTypeCode(type)));
    });
}

std::optional<ConnectToPeer> ConnectToPeer::decode(QByteArrayView payload)
{
    return parse<ConnectToPeer>(payload, [](Reader &r, ConnectToPeer &m) {
        m.token = r.u32();
        m.username = readText(r);
        const std::optional<ConnectionType> type = parseConnectionType(r.string());
        if (!type) {
            r.fail();
            return;
        }
        m.type = *type;
    });
}

QByteArray MessageUser::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.string(message);
    });
}

std::optional<MessageUser> MessageUser::decode(QByteArrayView payload)
{
    return parse<MessageUser>(payload, [](Reader &r, MessageUser &m) {
        m.username = readText(r);
        m.message = readText(r);
    });
}

QByteArray MessageAcked::encode() const
{
    return encodeWith([this](Writer &w) { w.u32(id); });
}

std::optional<MessageAcked> MessageAcked::decode(QByteArrayView payload)
{
    return parse<MessageAcked>(payload, [](Reader &r, MessageAcked &m) { m.id = r.u32(); });
}

QByteArray FileSearch::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(token);
        w.string(query);
    });
}

std::optional<FileSearch> FileSearch::decode(QByteArrayView payload)
{
    return parse<FileSearch>(payload, [](Reader &r, FileSearch &m) {
        m.token = r.u32();
        m.query = readText(r);
    });
}

QByteArray WishlistSearch::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(token);
        w.string(query);
    });
}

std::optional<WishlistSearch> WishlistSearch::decode(QByteArrayView payload)
{
    return parse<WishlistSearch>(payload, [](Reader &r, WishlistSearch &m) {
        m.token = r.u32();
        m.query = readText(r);
    });
}

QByteArray SetStatus::encode() const
{
    return encodeWith([this](Writer &w) { w.i32(status); });
}

std::optional<SetStatus> SetStatus::decode(QByteArrayView payload)
{
    return parse<SetStatus>(payload, [](Reader &r, SetStatus &m) { m.status = r.i32(); });
}

QByteArray ServerPing::encode() const
{
    return {};
}

std::optional<ServerPing> ServerPing::decode(QByteArrayView /*payload*/)
{
    return ServerPing{};
}

QByteArray RoomList::encode() const
{
    return {};
}

std::optional<RoomList> RoomList::decode(QByteArrayView /*payload*/)
{
    return RoomList{};
}

QByteArray CheckPrivileges::encode() const
{
    return {};
}

std::optional<CheckPrivileges> CheckPrivileges::decode(QByteArrayView /*payload*/)
{
    return CheckPrivileges{};
}

QByteArray SharedFoldersFiles::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(folders);
        w.u32(files);
    });
}

std::optional<SharedFoldersFiles> SharedFoldersFiles::decode(QByteArrayView payload)
{
    return parse<SharedFoldersFiles>(payload, [](Reader &r, SharedFoldersFiles &m) {
        m.folders = r.u32();
        m.files = r.u32();
    });
}

QByteArray UserSearch::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.u32(token);
        w.string(query);
    });
}

std::optional<UserSearch> UserSearch::decode(QByteArrayView payload)
{
    return parse<UserSearch>(payload, [](Reader &r, UserSearch &m) {
        m.username = readText(r);
        m.token = r.u32();
        m.query = readText(r);
    });
}

QByteArray HaveNoParent::encode() const
{
    return internal::encodeSingle<&HaveNoParent::noParent>(*this);
}

std::optional<HaveNoParent> HaveNoParent::decode(QByteArrayView payload)
{
    return internal::decodeSingle<HaveNoParent, &HaveNoParent::noParent>(payload);
}

QByteArray AcceptChildren::encode() const
{
    return internal::encodeSingle<&AcceptChildren::accept>(*this);
}

std::optional<AcceptChildren> AcceptChildren::decode(QByteArrayView payload)
{
    return internal::decodeSingle<AcceptChildren, &AcceptChildren::accept>(payload);
}

QByteArray EnableRoomInvitations::encode() const
{
    return internal::encodeSingle<&EnableRoomInvitations::enable>(*this);
}

std::optional<EnableRoomInvitations> EnableRoomInvitations::decode(QByteArrayView payload)
{
    return internal::decodeSingle<EnableRoomInvitations, &EnableRoomInvitations::enable>(payload);
}

QByteArray SendUploadSpeed::encode() const
{
    return internal::encodeSingle<&SendUploadSpeed::bytesPerSecond>(*this);
}

std::optional<SendUploadSpeed> SendUploadSpeed::decode(QByteArrayView payload)
{
    return internal::decodeSingle<SendUploadSpeed, &SendUploadSpeed::bytesPerSecond>(payload);
}

QByteArray BranchLevel::encode() const
{
    return internal::encodeSingle<&BranchLevel::level>(*this);
}

std::optional<BranchLevel> BranchLevel::decode(QByteArrayView payload)
{
    return internal::decodeSingle<BranchLevel, &BranchLevel::level>(payload);
}

QByteArray BranchRoot::encode() const
{
    return encodeWith([this](Writer &w) { w.string(root); });
}

std::optional<BranchRoot> BranchRoot::decode(QByteArrayView payload)
{
    return parse<BranchRoot>(payload, [](Reader &r, BranchRoot &m) { m.root = readText(r); });
}

QByteArray CantConnectToPeer::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(token);
        w.string(username);
    });
}

std::optional<CantConnectToPeer> CantConnectToPeer::decode(QByteArrayView payload)
{
    return parse<CantConnectToPeer>(payload, [](Reader &r, CantConnectToPeer &m) {
        m.token = r.u32();
        m.username = readText(r);
    });
}

} // namespace toserver

// --- fromserver ------------------------------------------------------------------------------------------------------
namespace fromserver {

QByteArray LoginReply::encode() const
{
    return encodeWith([this](Writer &w) {
        w.boolean(success);
        if (success) {
            w.string(greeting);
            w.u32(ownIp);
            w.string(QByteArrayView(passwordHash));
            w.boolean(isSupporter);
        } else {
            w.string(reason);
            if (reason == u"INVALIDUSERNAME"_s || !detail.isEmpty()) {
                w.string(detail);
            }
        }
    });
}

std::optional<LoginReply> LoginReply::decode(QByteArrayView payload)
{
    return parse<LoginReply>(payload, [](Reader &r, LoginReply &m) {
        m.success = r.boolean();
        if (m.success) {
            m.greeting = readText(r);
            m.ownIp = r.u32();
            m.passwordHash =
                readOptional<QByteArray>(r, [](Reader &probe) { return probe.string(); }).value_or(QByteArray());
            if (!r.atEnd()) {
                m.isSupporter = r.boolean();
            }
        } else {
            m.reason = readText(r);
            m.detail = readOptional<QString>(r, readText).value_or(QString());
        }
    });
}

QByteArray PeerAddress::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.u32(ip);
        w.u32(port);
        if (obfuscationType || obfuscatedPort) {
            w.u32(obfuscationType.value_or(0));
            w.u16(obfuscatedPort.value_or(0));
        }
    });
}

std::optional<PeerAddress> PeerAddress::decode(QByteArrayView payload)
{
    return parse<PeerAddress>(payload, [](Reader &r, PeerAddress &m) {
        m.username = readText(r);
        m.ip = r.u32();
        m.port = r.u32();
        if (r.remaining() >= 6) {
            m.obfuscationType = r.u32();
            m.obfuscatedPort = r.u16();
        }
    });
}

QByteArray WatchUserReply::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.boolean(exists);
        if (exists) {
            w.u32(static_cast<quint32>(status));
            writeStats(w, stats);
            if (isPresent(status)) {
                w.string(countryCode);
            }
        }
    });
}

std::optional<WatchUserReply> WatchUserReply::decode(QByteArrayView payload)
{
    return parse<WatchUserReply>(payload, [](Reader &r, WatchUserReply &m) {
        m.username = readText(r);
        m.exists = r.boolean();
        if (m.exists) {
            m.status = static_cast<UserStatus>(r.u32());
            m.stats = readStats(r);
            if (isPresent(m.status)) {
                m.countryCode = readOptional<QString>(r, readText).value_or(QString());
            }
        }
    });
}

QByteArray UserStatusUpdate::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.u32(static_cast<quint32>(status));
        w.boolean(privileged);
    });
}

std::optional<UserStatusUpdate> UserStatusUpdate::decode(QByteArrayView payload)
{
    return parse<UserStatusUpdate>(payload, [](Reader &r, UserStatusUpdate &m) {
        m.username = readText(r);
        m.status = static_cast<UserStatus>(r.u32());
        if (!r.atEnd()) {
            m.privileged = r.boolean();
        }
    });
}

QByteArray RoomMessage::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(room);
        w.string(username);
        w.string(message);
    });
}

std::optional<RoomMessage> RoomMessage::decode(QByteArrayView payload)
{
    return parse<RoomMessage>(payload, [](Reader &r, RoomMessage &m) {
        m.room = readText(r);
        m.username = readText(r);
        m.message = readText(r);
    });
}

QByteArray RoomJoined::encode() const
{
    return encodeWith([this](Writer &w) {
        const auto n = static_cast<quint32>(members.size());
        w.string(room);
        w.u32(n);
        for (const RoomMember &member : members) {
            w.string(member.username);
        }
        w.u32(n);
        for (const RoomMember &member : members) {
            w.u32(static_cast<quint32>(member.status));
        }
        w.u32(n);
        for (const RoomMember &member : members) {
            writeStats(w, member.stats);
        }
        w.u32(n);
        for (const RoomMember &member : members) {
            w.u32(member.slotsFull);
        }
        w.u32(n);
        for (const RoomMember &member : members) {
            w.string(member.countryCode);
        }
        if (owner) {
            w.string(*owner);
            writeStringList(w, operators);
        }
    });
}

std::optional<RoomJoined> RoomJoined::decode(QByteArrayView payload)
{
    // The smallest a member can take across the five lists: a 1-byte name (5), status (4), stats (20), slots (4) and
    // an empty country (4). Checked before anything is reserved.
    constexpr qsizetype kMinMemberBytes = 37;
    return parse<RoomJoined>(payload, [](Reader &r, RoomJoined &m) {
        m.room = readText(r);
        const qsizetype n = r.count(kMinMemberBytes);
        if (n > protocol::kRoomMembersMax) {
            r.fail();
            return;
        }
        m.members.resize(n);
        for (RoomMember &member : m.members) {
            member.username = readText(r);
        }
        // Every parallel list must have the same count.
        const auto sameCount = [&r, n] {
            if (std::cmp_not_equal(r.u32(), n)) {
                r.fail();
            }
            return r.ok();
        };
        if (!sameCount()) {
            return;
        }
        for (RoomMember &member : m.members) {
            member.status = static_cast<UserStatus>(r.u32());
        }
        if (!sameCount()) {
            return;
        }
        for (RoomMember &member : m.members) {
            member.stats = readStats(r);
        }
        if (!sameCount()) {
            return;
        }
        for (RoomMember &member : m.members) {
            member.slotsFull = r.u32();
        }
        if (!sameCount()) {
            return;
        }
        for (RoomMember &member : m.members) {
            member.countryCode = readText(r);
        }
        // Private rooms only: the owner and the operators.
        using OwnerAndOperators = std::pair<QString, QStringList>;
        const std::optional<OwnerAndOperators> privateRoom = readOptional<OwnerAndOperators>(r, [](Reader &probe) {
            QString owner = readText(probe);
            QStringList operators = readStringList(probe);
            return OwnerAndOperators{std::move(owner), std::move(operators)};
        });
        if (privateRoom) {
            m.owner = privateRoom->first;
            m.operators = privateRoom->second;
        }
    });
}

QByteArray RoomLeft::encode() const
{
    return encodeWith([this](Writer &w) { w.string(room); });
}

std::optional<RoomLeft> RoomLeft::decode(QByteArrayView payload)
{
    return parse<RoomLeft>(payload, [](Reader &r, RoomLeft &m) { m.room = readText(r); });
}

QByteArray RoomUserJoined::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(room);
        w.string(member.username);
        w.u32(static_cast<quint32>(member.status));
        writeStats(w, member.stats);
        w.u32(member.slotsFull);
        w.string(member.countryCode);
    });
}

std::optional<RoomUserJoined> RoomUserJoined::decode(QByteArrayView payload)
{
    return parse<RoomUserJoined>(payload, [](Reader &r, RoomUserJoined &m) {
        m.room = readText(r);
        m.member.username = readText(r);
        m.member.status = static_cast<UserStatus>(r.u32());
        m.member.stats = readStats(r);
        m.member.slotsFull = r.u32();
        m.member.countryCode = readOptional<QString>(r, readText).value_or(QString());
    });
}

QByteArray RoomUserLeft::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(room);
        w.string(username);
    });
}

std::optional<RoomUserLeft> RoomUserLeft::decode(QByteArrayView payload)
{
    return parse<RoomUserLeft>(payload, [](Reader &r, RoomUserLeft &m) {
        m.room = readText(r);
        m.username = readText(r);
    });
}

QByteArray ConnectToPeerRequest::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.string(QByteArrayView(connectionTypeCode(type)));
        w.u32(ip);
        w.u32(port);
        w.u32(token);
        w.boolean(privileged);
        if (obfuscationType || obfuscatedPort) {
            w.u32(obfuscationType.value_or(0));
            w.u32(obfuscatedPort.value_or(0));
        }
    });
}

std::optional<ConnectToPeerRequest> ConnectToPeerRequest::decode(QByteArrayView payload)
{
    return parse<ConnectToPeerRequest>(payload, [](Reader &r, ConnectToPeerRequest &m) {
        m.username = readText(r);
        const std::optional<ConnectionType> type = parseConnectionType(r.string());
        if (!type) {
            r.fail();
            return;
        }
        m.type = *type;
        m.ip = r.u32();
        m.port = r.u32();
        m.token = r.u32();
        if (!r.atEnd()) {
            m.privileged = r.boolean();
        }
        if (r.remaining() >= 8) {
            m.obfuscationType = r.u32();
            m.obfuscatedPort = r.u32();
        }
    });
}

QByteArray PrivateMessage::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(id);
        w.u32(timestamp);
        w.string(username);
        w.string(message);
        w.boolean(isNew);
    });
}

std::optional<PrivateMessage> PrivateMessage::decode(QByteArrayView payload)
{
    return parse<PrivateMessage>(payload, [](Reader &r, PrivateMessage &m) {
        m.id = r.u32();
        m.timestamp = r.u32();
        m.username = readText(r);
        m.message = readText(r);
        if (!r.atEnd()) {
            m.isNew = r.boolean();
        }
    });
}

QByteArray SearchRequest::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.u32(token);
        w.string(query);
    });
}

std::optional<SearchRequest> SearchRequest::decode(QByteArrayView payload)
{
    return parse<SearchRequest>(payload, [](Reader &r, SearchRequest &m) {
        m.username = readText(r);
        m.token = r.u32();
        m.query = readText(r);
    });
}

QByteArray UserStatsUpdate::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        writeStats(w, stats);
    });
}

std::optional<UserStatsUpdate> UserStatsUpdate::decode(QByteArrayView payload)
{
    return parse<UserStatsUpdate>(payload, [](Reader &r, UserStatsUpdate &m) {
        m.username = readText(r);
        m.stats = readStats(r);
    });
}

QByteArray Relogged::encode() const
{
    return {};
}

std::optional<Relogged> Relogged::decode(QByteArrayView /*payload*/)
{
    return Relogged{};
}

QByteArray ResetDistributed::encode() const
{
    return {};
}

std::optional<ResetDistributed> ResetDistributed::decode(QByteArrayView /*payload*/)
{
    return ResetDistributed{};
}

QByteArray RoomList::encode() const
{
    return encodeWith([this](Writer &w) {
        writeRoomEntries(w, publicRooms);
        writeRoomEntries(w, ownedPrivateRooms);
        writeRoomEntries(w, memberPrivateRooms);
        writeStringList(w, operatedPrivateRooms);
    });
}

std::optional<RoomList> RoomList::decode(QByteArrayView payload)
{
    return parse<RoomList>(payload, [](Reader &r, RoomList &m) {
        m.publicRooms = readRoomEntries(r);
        // The private-room lists are decoded and validated but may be absent.
        m.ownedPrivateRooms = readOptional<QList<RoomListEntry>>(r, readRoomEntries).value_or(QList<RoomListEntry>());
        m.memberPrivateRooms = readOptional<QList<RoomListEntry>>(r, readRoomEntries).value_or(QList<RoomListEntry>());
        m.operatedPrivateRooms = readOptional<QStringList>(r, [](Reader &probe) {
                                     QStringList names = readStringList(probe);
                                     if (names.size() > protocol::kRoomListMax) {
                                         probe.fail();
                                     }
                                     return names;
                                 }).value_or(QStringList());
    });
}

QByteArray AdminMessage::encode() const
{
    return encodeWith([this](Writer &w) { w.string(message); });
}

std::optional<AdminMessage> AdminMessage::decode(QByteArrayView payload)
{
    return parse<AdminMessage>(payload, [](Reader &r, AdminMessage &m) { m.message = readText(r); });
}

QByteArray PrivilegedUsers::encode() const
{
    return encodeWith([this](Writer &w) { writeStringList(w, users); });
}

std::optional<PrivilegedUsers> PrivilegedUsers::decode(QByteArrayView payload)
{
    return parse<PrivilegedUsers>(payload, [](Reader &r, PrivilegedUsers &m) { m.users = readStringList(r); });
}

QByteArray ParentMinSpeed::encode() const
{
    return internal::encodeSingle<&ParentMinSpeed::speed>(*this);
}

std::optional<ParentMinSpeed> ParentMinSpeed::decode(QByteArrayView payload)
{
    return internal::decodeSingle<ParentMinSpeed, &ParentMinSpeed::speed>(payload);
}

QByteArray ParentSpeedRatio::encode() const
{
    return internal::encodeSingle<&ParentSpeedRatio::ratio>(*this);
}

std::optional<ParentSpeedRatio> ParentSpeedRatio::decode(QByteArrayView payload)
{
    return internal::decodeSingle<ParentSpeedRatio, &ParentSpeedRatio::ratio>(payload);
}

QByteArray PrivilegesLeft::encode() const
{
    return internal::encodeSingle<&PrivilegesLeft::seconds>(*this);
}

std::optional<PrivilegesLeft> PrivilegesLeft::decode(QByteArrayView payload)
{
    return internal::decodeSingle<PrivilegesLeft, &PrivilegesLeft::seconds>(payload);
}

QByteArray WishlistInterval::encode() const
{
    return internal::encodeSingle<&WishlistInterval::seconds>(*this);
}

std::optional<WishlistInterval> WishlistInterval::decode(QByteArrayView payload)
{
    return internal::decodeSingle<WishlistInterval, &WishlistInterval::seconds>(payload);
}

QByteArray RoomInvitationsEnabled::encode() const
{
    return internal::encodeSingle<&RoomInvitationsEnabled::enabled>(*this);
}

std::optional<RoomInvitationsEnabled> RoomInvitationsEnabled::decode(QByteArrayView payload)
{
    return internal::decodeSingle<RoomInvitationsEnabled, &RoomInvitationsEnabled::enabled>(payload);
}

QByteArray EmbeddedMessage::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u8(distribCode);
        w.bytes(QByteArrayView(payload));
    });
}

std::optional<EmbeddedMessage> EmbeddedMessage::decode(QByteArrayView payload)
{
    return parse<EmbeddedMessage>(payload, [](Reader &r, EmbeddedMessage &m) {
        m.distribCode = r.u8();
        m.payload = r.rest().toByteArray();
    });
}

QByteArray PossibleParents::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(static_cast<quint32>(candidates.size()));
        for (const ParentCandidate &candidate : candidates) {
            w.string(candidate.username);
            w.u32(candidate.ip);
            w.u32(candidate.port);
        }
    });
}

std::optional<PossibleParents> PossibleParents::decode(QByteArrayView payload)
{
    return parse<PossibleParents>(payload, [](Reader &r, PossibleParents &m) {
        const qsizetype n = r.count(12);
        if (n > protocol::kPossibleParentsMax) {
            r.fail();
            return;
        }
        m.candidates.reserve(n);
        for (qsizetype i = 0; i < n && r.ok(); ++i) {
            ParentCandidate candidate;
            candidate.username = readText(r);
            candidate.ip = r.u32();
            candidate.port = r.u32();
            m.candidates.append(std::move(candidate));
        }
    });
}

QByteArray ExcludedSearchPhrases::encode() const
{
    return encodeWith([this](Writer &w) { writeStringList(w, phrases); });
}

std::optional<ExcludedSearchPhrases> ExcludedSearchPhrases::decode(QByteArrayView payload)
{
    return parse<ExcludedSearchPhrases>(payload,
                                        [](Reader &r, ExcludedSearchPhrases &m) { m.phrases = readStringList(r); });
}

QByteArray CantConnectToPeer::encode() const
{
    return encodeWith([this](Writer &w) { w.u32(token); });
}

std::optional<CantConnectToPeer> CantConnectToPeer::decode(QByteArrayView payload)
{
    return parse<CantConnectToPeer>(payload, [](Reader &r, CantConnectToPeer &m) { m.token = r.u32(); });
}

QByteArray CantCreateRoom::encode() const
{
    return encodeWith([this](Writer &w) { w.string(room); });
}

std::optional<CantCreateRoom> CantCreateRoom::decode(QByteArrayView payload)
{
    return parse<CantCreateRoom>(payload, [](Reader &r, CantCreateRoom &m) { m.room = readText(r); });
}

} // namespace fromserver

// --- dispatch --------------------------------------------------------------------------------------------------------

bool isKnownServerCode(quint32 code)
{
    switch (static_cast<ServerCode>(code)) {
    case ServerCode::Login:
    case ServerCode::SetWaitPort:
    case ServerCode::GetPeerAddress:
    case ServerCode::WatchUser:
    case ServerCode::UnwatchUser:
    case ServerCode::GetUserStatus:
    case ServerCode::IgnoreUser:
    case ServerCode::UnignoreUser:
    case ServerCode::SayChatroom:
    case ServerCode::JoinRoom:
    case ServerCode::LeaveRoom:
    case ServerCode::UserJoinedRoom:
    case ServerCode::UserLeftRoom:
    case ServerCode::ConnectToPeer:
    case ServerCode::MessageUser:
    case ServerCode::MessageAcked:
    case ServerCode::FileSearchRoom:
    case ServerCode::FileSearch:
    case ServerCode::SetStatus:
    case ServerCode::ServerPing:
    case ServerCode::SendConnectToken:
    case ServerCode::SendDownloadSpeed:
    case ServerCode::SharedFoldersFiles:
    case ServerCode::GetUserStats:
    case ServerCode::UploadSlotsFull:
    case ServerCode::Relogged:
    case ServerCode::UserSearch:
    case ServerCode::SimilarRecommendations:
    case ServerCode::AddThingILike:
    case ServerCode::RemoveThingILike:
    case ServerCode::Recommendations:
    case ServerCode::MyRecommendations:
    case ServerCode::GlobalRecommendations:
    case ServerCode::UserInterests:
    case ServerCode::AdminCommand:
    case ServerCode::PlaceInLineRequest:
    case ServerCode::PlaceInLineResponse:
    case ServerCode::RoomAdded:
    case ServerCode::RoomRemoved:
    case ServerCode::RoomList:
    case ServerCode::ExactFileSearch:
    case ServerCode::AdminMessage:
    case ServerCode::GlobalUserList:
    case ServerCode::TunneledMessage:
    case ServerCode::PrivilegedUsers:
    case ServerCode::HaveNoParent:
    case ServerCode::ParentIp:
    case ServerCode::ParentMinSpeed:
    case ServerCode::ParentSpeedRatio:
    case ServerCode::ParentInactivityTimeout:
    case ServerCode::SearchInactivityTimeout:
    case ServerCode::MinParentsInCache:
    case ServerCode::DistribPingInterval:
    case ServerCode::AddToPrivileged:
    case ServerCode::CheckPrivileges:
    case ServerCode::EmbeddedMessage:
    case ServerCode::AcceptChildren:
    case ServerCode::PossibleParents:
    case ServerCode::WishlistSearch:
    case ServerCode::WishlistInterval:
    case ServerCode::SimilarUsers:
    case ServerCode::ItemRecommendations:
    case ServerCode::ItemSimilarUsers:
    case ServerCode::RoomTickers:
    case ServerCode::RoomTickerAdded:
    case ServerCode::RoomTickerRemoved:
    case ServerCode::SetRoomTicker:
    case ServerCode::AddThingIHate:
    case ServerCode::RemoveThingIHate:
    case ServerCode::RoomSearch:
    case ServerCode::SendUploadSpeed:
    case ServerCode::UserPrivileged:
    case ServerCode::GivePrivileges:
    case ServerCode::NotifyPrivileges:
    case ServerCode::AckNotifyPrivileges:
    case ServerCode::BranchLevel:
    case ServerCode::BranchRoot:
    case ServerCode::ChildDepth:
    case ServerCode::ResetDistributed:
    case ServerCode::RoomMembers:
    case ServerCode::AddRoomMember:
    case ServerCode::RemoveRoomMember:
    case ServerCode::CancelRoomMembership:
    case ServerCode::CancelRoomOwnership:
    case ServerCode::RoomSomething:
    case ServerCode::RoomMembershipGranted:
    case ServerCode::RoomMembershipRevoked:
    case ServerCode::EnableRoomInvitations:
    case ServerCode::ChangePassword:
    case ServerCode::AddRoomOperator:
    case ServerCode::RemoveRoomOperator:
    case ServerCode::RoomOperatorshipGranted:
    case ServerCode::RoomOperatorshipRevoked:
    case ServerCode::RoomOperators:
    case ServerCode::MessageUsers:
    case ServerCode::JoinGlobalRoom:
    case ServerCode::LeaveGlobalRoom:
    case ServerCode::GlobalRoomMessage:
    case ServerCode::RelatedSearch:
    case ServerCode::ExcludedSearchPhrases:
    case ServerCode::CantConnectToPeer:
    case ServerCode::CantCreateRoom:
        return true;
    }
    return false;
}

Decoded<ToServerMessage> decodeToServer(quint32 code, QByteArrayView payload)
{
    using internal::decodeAs;
    using V = ToServerMessage;
    switch (static_cast<ServerCode>(code)) {
    case ServerCode::Login:
        return decodeAs<V, toserver::Login>(payload);
    case ServerCode::SetWaitPort:
        return decodeAs<V, toserver::SetWaitPort>(payload);
    case ServerCode::GetPeerAddress:
        return decodeAs<V, toserver::GetPeerAddress>(payload);
    case ServerCode::WatchUser:
        return decodeAs<V, toserver::WatchUser>(payload);
    case ServerCode::UnwatchUser:
        return decodeAs<V, toserver::UnwatchUser>(payload);
    case ServerCode::GetUserStatus:
        return decodeAs<V, toserver::GetUserStatus>(payload);
    case ServerCode::SayChatroom:
        return decodeAs<V, toserver::SayChatroom>(payload);
    case ServerCode::JoinRoom:
        return decodeAs<V, toserver::JoinRoom>(payload);
    case ServerCode::LeaveRoom:
        return decodeAs<V, toserver::LeaveRoom>(payload);
    case ServerCode::ConnectToPeer:
        return decodeAs<V, toserver::ConnectToPeer>(payload);
    case ServerCode::MessageUser:
        return decodeAs<V, toserver::MessageUser>(payload);
    case ServerCode::MessageAcked:
        return decodeAs<V, toserver::MessageAcked>(payload);
    case ServerCode::FileSearch:
        return decodeAs<V, toserver::FileSearch>(payload);
    case ServerCode::SetStatus:
        return decodeAs<V, toserver::SetStatus>(payload);
    case ServerCode::ServerPing:
        return decodeAs<V, toserver::ServerPing>(payload);
    case ServerCode::SharedFoldersFiles:
        return decodeAs<V, toserver::SharedFoldersFiles>(payload);
    case ServerCode::GetUserStats:
        return decodeAs<V, toserver::GetUserStats>(payload);
    case ServerCode::UserSearch:
        return decodeAs<V, toserver::UserSearch>(payload);
    case ServerCode::RoomList:
        return decodeAs<V, toserver::RoomList>(payload);
    case ServerCode::HaveNoParent:
        return decodeAs<V, toserver::HaveNoParent>(payload);
    case ServerCode::CheckPrivileges:
        return decodeAs<V, toserver::CheckPrivileges>(payload);
    case ServerCode::AcceptChildren:
        return decodeAs<V, toserver::AcceptChildren>(payload);
    case ServerCode::WishlistSearch:
        return decodeAs<V, toserver::WishlistSearch>(payload);
    case ServerCode::SendUploadSpeed:
        return decodeAs<V, toserver::SendUploadSpeed>(payload);
    case ServerCode::BranchLevel:
        return decodeAs<V, toserver::BranchLevel>(payload);
    case ServerCode::BranchRoot:
        return decodeAs<V, toserver::BranchRoot>(payload);
    case ServerCode::EnableRoomInvitations:
        return decodeAs<V, toserver::EnableRoomInvitations>(payload);
    case ServerCode::CantConnectToPeer:
        return decodeAs<V, toserver::CantConnectToPeer>(payload);
    default:
        break;
    }
    return {isKnownServerCode(code) ? DecodeStatus::Ignored : DecodeStatus::Unknown, std::nullopt};
}

Decoded<FromServerMessage> decodeFromServer(quint32 code, QByteArrayView payload)
{
    using internal::decodeAs;
    using V = FromServerMessage;
    switch (static_cast<ServerCode>(code)) {
    case ServerCode::Login:
        return decodeAs<V, fromserver::LoginReply>(payload);
    case ServerCode::GetPeerAddress:
        return decodeAs<V, fromserver::PeerAddress>(payload);
    case ServerCode::WatchUser:
        return decodeAs<V, fromserver::WatchUserReply>(payload);
    case ServerCode::GetUserStatus:
        return decodeAs<V, fromserver::UserStatusUpdate>(payload);
    case ServerCode::SayChatroom:
        return decodeAs<V, fromserver::RoomMessage>(payload);
    case ServerCode::JoinRoom:
        return decodeAs<V, fromserver::RoomJoined>(payload);
    case ServerCode::LeaveRoom:
        return decodeAs<V, fromserver::RoomLeft>(payload);
    case ServerCode::UserJoinedRoom:
        return decodeAs<V, fromserver::RoomUserJoined>(payload);
    case ServerCode::UserLeftRoom:
        return decodeAs<V, fromserver::RoomUserLeft>(payload);
    case ServerCode::ConnectToPeer:
        return decodeAs<V, fromserver::ConnectToPeerRequest>(payload);
    case ServerCode::MessageUser:
        return decodeAs<V, fromserver::PrivateMessage>(payload);
    case ServerCode::FileSearch:
        return decodeAs<V, fromserver::SearchRequest>(payload);
    case ServerCode::GetUserStats:
        return decodeAs<V, fromserver::UserStatsUpdate>(payload);
    case ServerCode::Relogged:
        return decodeAs<V, fromserver::Relogged>(payload);
    case ServerCode::RoomList:
        return decodeAs<V, fromserver::RoomList>(payload);
    case ServerCode::AdminMessage:
        return decodeAs<V, fromserver::AdminMessage>(payload);
    case ServerCode::PrivilegedUsers:
        return decodeAs<V, fromserver::PrivilegedUsers>(payload);
    case ServerCode::ParentMinSpeed:
        return decodeAs<V, fromserver::ParentMinSpeed>(payload);
    case ServerCode::ParentSpeedRatio:
        return decodeAs<V, fromserver::ParentSpeedRatio>(payload);
    case ServerCode::CheckPrivileges:
        return decodeAs<V, fromserver::PrivilegesLeft>(payload);
    case ServerCode::EmbeddedMessage:
        return decodeAs<V, fromserver::EmbeddedMessage>(payload);
    case ServerCode::PossibleParents:
        return decodeAs<V, fromserver::PossibleParents>(payload);
    case ServerCode::WishlistInterval:
        return decodeAs<V, fromserver::WishlistInterval>(payload);
    case ServerCode::ResetDistributed:
        return decodeAs<V, fromserver::ResetDistributed>(payload);
    case ServerCode::EnableRoomInvitations:
        return decodeAs<V, fromserver::RoomInvitationsEnabled>(payload);
    case ServerCode::ExcludedSearchPhrases:
        return decodeAs<V, fromserver::ExcludedSearchPhrases>(payload);
    case ServerCode::CantConnectToPeer:
        return decodeAs<V, fromserver::CantConnectToPeer>(payload);
    case ServerCode::CantCreateRoom:
        return decodeAs<V, fromserver::CantCreateRoom>(payload);
    default:
        break;
    }
    return {isKnownServerCode(code) ? DecodeStatus::Ignored : DecodeStatus::Unknown, std::nullopt};
}

} // namespace selecta::slsk
