// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"

#include <QString>

#include <functional>
#include <optional>

namespace selecta::slsk {

// Why slsk wants to open a new outbound peer connection. The governor may budget these differently: connections
// triggered by other users (answering their searches, uploading to them) shouldn't starve the user's own work, and
// vice versa (sharing note §8).
enum class ConnectPurpose {
    Request,        // our own request to that user: browse, user info, folder contents, queueing a download
    SearchResponse, // delivering our answer to their search
    Transfer,       // a transfer: TransferRequest on `P`, or the `F` connection of an upload
    Distributed,    // a distributed parent candidate
};

const char *connectPurposeName(ConnectPurpose purpose);

// Where slsk gets the PeerLookup permits that new outbound connections need (every new outbound connection costs one
// lookup: GetPeerAddress and ConnectToPeer, protocol §3.2). Implemented by core/ on top of governor::Governor; tests
// implement it with governor::testing::PermitFactory.
//
// slsk asks only when it must open a connection: requests to a user it is already connected to don't ask. Answering
// another user's indirect connection request (dialling back with PierceFireWall) sends no lookup and isn't budgeted.
//
// Threading: called on the engine thread. core/ runs the callback on the engine thread exactly once, on a later
// event-loop turn (never re-entrantly from requestPeerLookup()). slsk keeps the request pending until then, tolerates
// a callback that arrives after the requester is gone (the callback holds no raw pointer), and drops a permit that
// arrives after the request was given up. Tests may also call back synchronously; slsk copes with that too.
class PermitSource
{
public:
    using Callback = std::function<void(std::optional<governor::Permit> permit)>;

    PermitSource() = default;
    PermitSource(const PermitSource &) = delete;
    PermitSource &operator=(const PermitSource &) = delete;
    virtual ~PermitSource();

    // Asks for a governor::Operation::PeerLookup permit to connect to `username`. Deliver the permit, or nullopt to
    // refuse (the waiting requests then fail with PeerError::NotPermitted).
    virtual void requestPeerLookup(const QString &username, ConnectPurpose purpose, Callback deliver) = 0;
};

} // namespace selecta::slsk
