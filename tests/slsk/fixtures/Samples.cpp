// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures/Samples.h"

#include "slsk/Frame.h"

using namespace Qt::StringLiterals;

namespace selecta::slsk::testing {

FileEntry sampleFile(int index)
{
    FileEntry file;
    switch (index % 4) {
    case 0: // a VBR MP3 with every lossy attribute
        file.name = RemotePath::fromText(u"%1 - Tídewater (Original Mix).mp3"_s.arg(index));
        file.size = 9'876'543;
        file.extension = u"mp3"_s;
        file.attributes.bitrate = 320;
        file.attributes.duration = 412;
        file.attributes.vbr = true;
        break;
    case 1: // lossless, SoulseekQt style: no extension
        file.name = RemotePath{QByteArray("0") + QByteArray::number(index) + QByteArray(" Hom\xe9.flac")};
        file.size = 5'000'000'000ULL; // over 4 GiB
        file.attributes.duration = 3600;
        file.attributes.sampleRate = 96000;
        file.attributes.bitDepth = 24;
        break;
    case 2: // no attributes at all
        file.name = RemotePath::fromText(u"cover%1.jpg"_s.arg(index));
        file.size = 0;
        break;
    default: // unknown and obsolete attributes kept
        file.name = RemotePath::fromText(u"日本の曲 %1.wv"_s.arg(index));
        file.size = 1;
        file.extension = u"wv"_s;
        file.attributes.bitrate = 900;
        file.attributes.vbr = false;
        file.attributes.other = {{3, 1}, {77, 0xFFFFFFFF}};
        break;
    }
    return file;
}

SharedDirectory sampleDirectory(int index, int files)
{
    SharedDirectory directory;
    directory.path = RemotePath::fromText(u"Collection\\Artist %1\\Album (2020)"_s.arg(index));
    for (int i = 0; i < files; ++i) {
        directory.files.append(sampleFile((index * 31) + i));
    }
    return directory;
}

SharedFileList sampleShareList(int directories, int filesPerDirectory)
{
    SharedFileList list;
    for (int i = 0; i < directories; ++i) {
        list.directories.append(sampleDirectory(i, filesPerDirectory));
    }
    list.directories.append(SharedDirectory{RemotePath::fromText(u"Collection\\Empty folder"_s), {}});
    list.privateDirectories.append(sampleDirectory(99, 2));
    return list;
}

QList<ToServerMessage> toServerSamples()
{
    QList<ToServerMessage> samples;
    samples.append(toserver::Login::make(u"username"_s, u"password"_s));
    samples.append(toserver::Login::make(u"Björk_fan"_s, u"pässwörd 🎵"_s));
    samples.append(toserver::SetWaitPort{2234, std::nullopt, std::nullopt});
    samples.append(toserver::SetWaitPort{2234, 1, 2235});
    samples.append(toserver::GetPeerAddress{u"bob"_s});
    samples.append(toserver::WatchUser{u"alice"_s});
    samples.append(toserver::UnwatchUser{u"alice"_s});
    samples.append(toserver::GetUserStatus{u"carol"_s});
    samples.append(toserver::SayChatroom{u"The Lobby"_s, u"héllo everyone"_s});
    samples.append(toserver::JoinRoom{u"nicotine"_s, 0});
    samples.append(toserver::LeaveRoom{u"nicotine"_s});
    samples.append(toserver::ConnectToPeer{0xDEADBEEF, u"dave"_s, ConnectionType::File});
    samples.append(toserver::ConnectToPeer{1, u"erin"_s, ConnectionType::Distributed});
    samples.append(toserver::MessageUser{u"frank"_s, u"hi there"_s});
    samples.append(toserver::MessageAcked{123456});
    samples.append(toserver::FileSearch{77, u"artist -live *mix"_s});
    samples.append(toserver::SetStatus{1});
    samples.append(toserver::ServerPing{});
    samples.append(toserver::SharedFoldersFiles{120, 4321});
    samples.append(toserver::GetUserStats{u"gina"_s});
    samples.append(toserver::UserSearch{u"hank"_s, 78, u"query"_s});
    samples.append(toserver::RoomList{});
    samples.append(toserver::HaveNoParent{true});
    samples.append(toserver::CheckPrivileges{});
    samples.append(toserver::AcceptChildren{true});
    samples.append(toserver::WishlistSearch{79, u"rare track"_s});
    samples.append(toserver::SendUploadSpeed{1'048'576});
    samples.append(toserver::BranchLevel{3});
    samples.append(toserver::BranchRoot{u"rooty"_s});
    samples.append(toserver::EnableRoomInvitations{false});
    samples.append(toserver::CantConnectToPeer{42, u"ivan"_s});
    return samples;
}

QList<FromServerMessage> fromServerSamples()
{
    QList<FromServerMessage> samples;
    fromserver::LoginReply ok;
    ok.success = true;
    ok.greeting = u"Welcome to Soulseek!"_s;
    ok.ownIp = 0x0A000001;
    ok.passwordHash = "5f4dcc3b5aa765d61d8327deb882cf99";
    ok.isSupporter = true;
    samples.append(ok);
    fromserver::LoginReply rejected;
    rejected.reason = u"INVALIDUSERNAME"_s;
    rejected.detail = u"Nick too long."_s;
    samples.append(rejected);
    fromserver::LoginReply wrongPass;
    wrongPass.reason = u"INVALIDPASS"_s;
    samples.append(wrongPass);
    samples.append(fromserver::PeerAddress{u"bob"_s, 0x01020304, 2234, std::nullopt, std::nullopt});
    samples.append(fromserver::PeerAddress{u"offline"_s, 0, 0, 0, 0});
    samples.append(fromserver::PeerAddress{u"obf"_s, 0xC0A80002, 2234, 1, 2235});
    fromserver::WatchUserReply watched;
    watched.username = u"alice"_s;
    watched.exists = true;
    watched.status = UserStatus::Online;
    watched.stats = UserStats{50'000, 12, 0, 3000, 120};
    watched.countryCode = u"FR"_s;
    samples.append(watched);
    fromserver::WatchUserReply away = watched;
    away.status = UserStatus::Offline;
    away.countryCode.clear(); // not sent when offline
    samples.append(away);
    samples.append(fromserver::WatchUserReply{u"ghost"_s, false, UserStatus::Offline, {}, {}});
    samples.append(fromserver::UserStatusUpdate{u"carol"_s, UserStatus::Away, true});
    samples.append(fromserver::RoomMessage{u"The Lobby"_s, u"dave"_s, u"anyone got this? 日本"_s});
    fromserver::RoomJoined room;
    room.room = u"The Lobby"_s;
    room.members.append(RoomMember{u"me"_s, UserStatus::Online, UserStats{1, 2, 3, 4, 5}, 0, u""_s});
    room.members.append(RoomMember{u"other"_s, UserStatus::Away, UserStats{10, 20, 30, 40, 50}, 1, u"DE"_s});
    samples.append(room);
    fromserver::RoomJoined privateRoom = room;
    privateRoom.room = u"secret"_s;
    privateRoom.owner = u"boss"_s;
    privateRoom.operators = {u"op1"_s, u"op2"_s};
    samples.append(privateRoom);
    samples.append(fromserver::RoomJoined{u"empty"_s, {}, std::nullopt, {}});
    samples.append(fromserver::RoomLeft{u"The Lobby"_s});
    samples.append(fromserver::RoomUserJoined{
        u"The Lobby"_s, RoomMember{u"newbie"_s, UserStatus::Online, UserStats{7, 8, 9, 10, 11}, 0, u"SE"_s}});
    samples.append(fromserver::RoomUserLeft{u"The Lobby"_s, u"newbie"_s});
    samples.append(fromserver::ConnectToPeerRequest{u"erin"_s, ConnectionType::Peer, 0x7F000001, 50300, 991, true,
                                                    std::nullopt, std::nullopt});
    samples.append(
        fromserver::ConnectToPeerRequest{u"fay"_s, ConnectionType::File, 0x0A000002, 2240, 992, false, 0, 0});
    samples.append(
        fromserver::PrivateMessage{5, 1'760'000'000, u"server"_s, u"You have been banned for 30 minutes."_s, true});
    samples.append(fromserver::PrivateMessage{6, 1'760'000'001, u"gus"_s, u"queued while away"_s, false});
    samples.append(fromserver::SearchRequest{u"hank"_s, 1234, u"artist track"_s});
    samples.append(fromserver::UserStatsUpdate{u"ida"_s, UserStats{100, 2, 0, 5000, 200}});
    samples.append(fromserver::Relogged{});
    fromserver::RoomList rooms;
    rooms.publicRooms = {{u"The Lobby"_s, 500}, {u"nicotine"_s, 120}};
    rooms.ownedPrivateRooms = {{u"mine"_s, 3}};
    rooms.memberPrivateRooms = {{u"theirs"_s, 9}};
    rooms.operatedPrivateRooms = {u"theirs"_s};
    samples.append(rooms);
    samples.append(fromserver::RoomList{{{u"only public"_s, 7}}, {}, {}, {}});
    samples.append(fromserver::AdminMessage{u"Server maintenance at 03:00"_s});
    samples.append(fromserver::PrivilegedUsers{{u"donor1"_s, u"donor2"_s}});
    samples.append(fromserver::ParentMinSpeed{1});
    samples.append(fromserver::ParentSpeedRatio{50});
    samples.append(fromserver::PrivilegesLeft{86400});
    samples.append(fromserver::EmbeddedMessage{3, distrib::Search{49, u"searcher"_s, 7, u"q"_s}.encode()});
    samples.append(fromserver::PossibleParents{
        {ParentCandidate{u"p1"_s, 0x0A000003, 2234}, ParentCandidate{u"p2"_s, 0x0A000004, 2235}}});
    samples.append(fromserver::WishlistInterval{720});
    samples.append(fromserver::ResetDistributed{});
    samples.append(fromserver::RoomInvitationsEnabled{false});
    samples.append(fromserver::ExcludedSearchPhrases{{u"bad phrase"_s, u"another"_s}});
    samples.append(fromserver::CantConnectToPeer{991});
    samples.append(fromserver::CantCreateRoom{u"secret"_s});
    return samples;
}

QList<PeerInitMessage> peerInitSamples()
{
    return {peerinit::PierceFireWall{0xFFFFFFFF}, peerinit::PeerInit{u"alice"_s, ConnectionType::Peer, 0},
            peerinit::PeerInit{u"Björk"_s, ConnectionType::File, 0},
            peerinit::PeerInit{u"dist"_s, ConnectionType::Distributed, 0}};
}

QList<PeerMessage> peerSamples()
{
    QList<PeerMessage> samples;
    samples.append(peer::GetShareFileList{});
    samples.append(peer::SharedFileListResponse{sampleShareList(3, 4)});
    samples.append(peer::SharedFileListResponse{SharedFileList{}});
    peer::FileSearchResponse results;
    results.username = u"responder"_s;
    results.token = 77;
    for (int i = 0; i < 6; ++i) {
        FileEntry file = sampleFile(i);
        file.name = RemotePath{QByteArray("Music\\Album\\") + file.name.raw}; // full virtual path
        results.results.append(file);
    }
    results.slotFree = true;
    results.averageSpeed = 250'000;
    results.queueLength = 3;
    results.privateResults.append(sampleFile(9));
    samples.append(results);
    samples.append(peer::UserInfoRequest{});
    samples.append(
        peer::UserInfoResponse{u"I share synthetic tones."_s, QByteArray("\x89PNG\r\n\x1a\n", 8), 3, 12, true, 0});
    samples.append(peer::UserInfoResponse{u""_s, std::nullopt, 0, 0, false, std::nullopt});
    samples.append(peer::FolderContentsRequest{55, RemotePath{QByteArray("Music\\Bj\xf6rk\\Debut")}});
    samples.append(
        peer::FolderContentsResponse{55, RemotePath{QByteArray("Music\\Bj\xf6rk\\Debut")}, {sampleDirectory(1, 5)}});
    samples.append(peer::FolderContentsResponse{56, RemotePath::fromText(u"unknown"_s), {}});
    samples.append(
        peer::TransferRequest{TransferDirection::Upload, 7, RemotePath::fromText(u"a\\b.flac"_s), 5'000'000'000ULL});
    samples.append(
        peer::TransferRequest{TransferDirection::Download, 8, RemotePath::fromText(u"a\\c.mp3"_s), std::nullopt});
    samples.append(peer::TransferResponse{7, true, std::nullopt, {}});
    samples.append(peer::TransferResponse{8, true, 1234, {}});
    samples.append(peer::TransferResponse{9, false, std::nullopt, u"Queued"_s});
    samples.append(peer::QueueUpload{RemotePath{QByteArray("Music\\Hom\xe9.mp3")}});
    samples.append(peer::PlaceInQueueResponse{RemotePath::fromText(u"x.mp3"_s), 14});
    samples.append(peer::UploadFailed{RemotePath::fromText(u"x.mp3"_s)});
    samples.append(peer::UploadDenied{RemotePath::fromText(u"x.mp3"_s), u"File not shared."_s});
    samples.append(peer::PlaceInQueueRequest{RemotePath::fromText(u"x.mp3"_s)});
    return samples;
}

QList<DistribMessage> distribSamples()
{
    return {distrib::Search{49, u"carol"_s, 0x01020304, u"abc"_s},
            distrib::Search{49, u"Ünïcode"_s, 1, u"tídewater -live"_s},
            distrib::BranchLevel{0},
            distrib::BranchLevel{4},
            distrib::BranchRoot{u"root"_s},
            distrib::EmbeddedMessage{3, distrib::Search{49, u"x"_s, 2, u"y"_s}.encode()}};
}

} // namespace selecta::slsk::testing
