// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Codec.h"
#include "slsk/Protocol.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>
#include <variant>

// Distributed (`D`) channel messages (references/soulseek-protocol.md §8). Each struct follows the convention of
// SELECTA_SLSK_MESSAGE (Codec.h). Pure and reentrant.
namespace selecta::slsk::distrib {

// A search travelling down the tree. Forwarded to children as the raw bytes received, never re-encoded.
struct Search
{
    SELECTA_SLSK_MESSAGE(Search, DistribCode::Search);
    quint32 identifier = protocol::kDistribSearchIdentifier; // decode() rejects any other value
    QString username;                                        // the searcher
    quint32 token = 0;
    QString query;
};

// The sender's depth in the tree; 0 means the sender is a branch root.
struct BranchLevel
{
    SELECTA_SLSK_MESSAGE(BranchLevel, DistribCode::BranchLevel);
    qint32 level = 0;
};

// The username at the root of the sender's branch.
struct BranchRoot
{
    SELECTA_SLSK_MESSAGE(BranchRoot, DistribCode::BranchRoot);
    QString root;
};

// A whole distributed message wrapped in another (older SoulseekQt roots). Only an embedded Search (code 3) means
// anything: unpack it and treat it as a Search.
struct EmbeddedMessage
{
    SELECTA_SLSK_MESSAGE(EmbeddedMessage, DistribCode::EmbeddedMessage);
    quint8 distribCode = 0;
    QByteArray payload;
};

} // namespace selecta::slsk::distrib

namespace selecta::slsk {

using DistribMessage =
    std::variant<distrib::Search, distrib::BranchLevel, distrib::BranchRoot, distrib::EmbeddedMessage>;

// Decodes one payload by code. Ping and ChildDepth are Ignored; other codes Unknown.
Decoded<DistribMessage> decodeDistrib(quint32 code, QByteArrayView payload);
bool isKnownDistribCode(quint32 code);

} // namespace selecta::slsk
