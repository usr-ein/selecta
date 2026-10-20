// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/PeerMessages.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QList>
#include <QString>

// What slsk asks core/ when a peer asks us something. slsk never decides what is shared and never touches files: it
// forwards the question and sends back the answer as it is given. Answers are unbudgeted (they travel on the
// connection the asking peer opened).
//
// Threading: called synchronously on the engine thread, so implementations must be fast (answer from the immutable
// share index, ADR-012). Ownership: core/ owns the implementations; they must outlive the PeerNetwork they are set
// on, or be unset first.
namespace selecta::slsk {

class ShareProvider
{
public:
    ShareProvider() = default;
    ShareProvider(const ShareProvider &) = delete;
    ShareProvider &operator=(const ShareProvider &) = delete;
    virtual ~ShareProvider();

    // The SharedFileListResponse payload `username` may see: a zlib stream as peer::SharedFileListResponse::encode()
    // produces it (the share index keeps these precompressed). Return the empty list's payload to refuse. A payload
    // larger than protocol::kShareListFrameMax is not sent.
    virtual QByteArray sharedFileList(const QString &username) = 0;

    // The contents of `folder` (the raw path the peer sent) that `username` may see: one directory with its files,
    // no subfolders. Empty for an unknown or forbidden folder; slsk then answers with an empty listing.
    virtual QList<SharedDirectory> folderContents(const QString &username, const RemotePath &folder) = 0;
};

class UserInfoProvider
{
public:
    UserInfoProvider() = default;
    UserInfoProvider(const UserInfoProvider &) = delete;
    UserInfoProvider &operator=(const UserInfoProvider &) = delete;
    virtual ~UserInfoProvider();

    // Our user info as `username` may see it: description, picture, upload slots, queue size, free slot. The same
    // numbers as our search answers (sharing note §5.2).
    virtual peer::UserInfoResponse userInfo(const QString &username) = 0;
};

} // namespace selecta::slsk
