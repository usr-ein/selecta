// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/MetaTypes.h"

#include "slsk/Client.h"
#include "slsk/DistributedNode.h"
#include "slsk/PeerNetwork.h"
#include "slsk/ServerSession.h"
#include "slsk/TransferManager.h"

#include <QTcpSocket>

#include <mutex>

namespace selecta::slsk {

void registerMetaTypes()
{
    static std::once_flag once;
    std::call_once(once, [] {
        qRegisterMetaType<RemotePath>();
        qRegisterMetaType<ConnectionType>();
        qRegisterMetaType<UserStatus>();
        qRegisterMetaType<UserStats>();
        qRegisterMetaType<FileEntry>();
        qRegisterMetaType<SharedDirectory>();
        qRegisterMetaType<SharedFileList>();
        qRegisterMetaType<Rejection>();
        qRegisterMetaType<RoomMember>();
        qRegisterMetaType<RoomListEntry>();
        qRegisterMetaType<ParentCandidate>();
        qRegisterMetaType<QList<ParentCandidate>>();
        qRegisterMetaType<fromserver::LoginReply>();
        qRegisterMetaType<fromserver::PeerAddress>();
        qRegisterMetaType<fromserver::WatchUserReply>();
        qRegisterMetaType<fromserver::UserStatusUpdate>();
        qRegisterMetaType<fromserver::RoomMessage>();
        qRegisterMetaType<fromserver::RoomJoined>();
        qRegisterMetaType<fromserver::RoomUserJoined>();
        qRegisterMetaType<fromserver::RoomUserLeft>();
        qRegisterMetaType<fromserver::ConnectToPeerRequest>();
        qRegisterMetaType<fromserver::PrivateMessage>();
        qRegisterMetaType<fromserver::SearchRequest>();
        qRegisterMetaType<fromserver::UserStatsUpdate>();
        qRegisterMetaType<fromserver::RoomList>();
        qRegisterMetaType<LoginInfo>();
        qRegisterMetaType<ServerNotice>();
        qRegisterMetaType<ServerSession::State>();
        qRegisterMetaType<ServerSession::EndReason>();
        qRegisterMetaType<TransferDirection>();
        qRegisterMetaType<peer::FileSearchResponse>();
        qRegisterMetaType<peer::UserInfoResponse>();
        qRegisterMetaType<peer::FolderContentsResponse>();
        qRegisterMetaType<peer::TransferRequest>();
        qRegisterMetaType<peer::TransferResponse>();
        qRegisterMetaType<peer::QueueUpload>();
        qRegisterMetaType<peer::PlaceInQueueRequest>();
        qRegisterMetaType<peer::PlaceInQueueResponse>();
        qRegisterMetaType<peer::UploadFailed>();
        qRegisterMetaType<peer::UploadDenied>();
        qRegisterMetaType<PeerError>();
        qRegisterMetaType<PeerRequest>();
        qRegisterMetaType<QTcpSocket *>();
        qRegisterMetaType<TransferId>("selecta::slsk::TransferId");
        qRegisterMetaType<DownloadState>();
        qRegisterMetaType<UploadState>();
        qRegisterMetaType<TransferError>();
        qRegisterMetaType<DownloadInfo>();
        qRegisterMetaType<UploadInfo>();
        qRegisterMetaType<distrib::Search>();
        qRegisterMetaType<IncomingSearch>();
    });
}

} // namespace selecta::slsk
