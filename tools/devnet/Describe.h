// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/PeerMessages.h"
#include "slsk/ServerMessages.h"

#include <QByteArrayView>
#include <QString>

namespace selecta::devnet {

// One line about a message, for the log: its name, then the fields worth reading (users, tokens, queries, paths,
// reasons, counts). Messages without such fields get their name alone.
QString describe(const slsk::ToServerMessage &message);
QString describe(const slsk::FromServerMessage &message);
QString describe(const slsk::PeerMessage &message);

// A frame as the server writes it to a client, or as a peer writes it on a P connection (u32 length, u32 code,
// payload): decoded and described, or its code and size when it doesn't decode.
QString describeServerFrame(QByteArrayView frame);
QString describePeerFrame(QByteArrayView frame);

} // namespace selecta::devnet
