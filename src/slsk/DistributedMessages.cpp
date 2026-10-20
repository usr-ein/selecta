// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/DistributedMessages.h"

#include "slsk/internal/CodecHelpers.h"

namespace selecta::slsk {

using internal::encodeWith;
using internal::parse;
using internal::readText;

namespace distrib {

QByteArray Search::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(identifier);
        w.string(username);
        w.u32(token);
        w.string(query);
    });
}

std::optional<Search> Search::decode(QByteArrayView payload)
{
    return parse<Search>(payload, [](Reader &r, Search &m) {
        m.identifier = r.u32();
        // Nicotine+ rejects any other identifier; so do we (it also catches misaligned garbage).
        if (m.identifier != protocol::kDistribSearchIdentifier) {
            r.fail();
            return;
        }
        m.username = readText(r);
        m.token = r.u32();
        m.query = readText(r);
    });
}

QByteArray BranchLevel::encode() const
{
    return encodeWith([this](Writer &w) { w.i32(level); });
}

std::optional<BranchLevel> BranchLevel::decode(QByteArrayView payload)
{
    return parse<BranchLevel>(payload, [](Reader &r, BranchLevel &m) { m.level = r.i32(); });
}

QByteArray BranchRoot::encode() const
{
    return encodeWith([this](Writer &w) { w.string(root); });
}

std::optional<BranchRoot> BranchRoot::decode(QByteArrayView payload)
{
    return parse<BranchRoot>(payload, [](Reader &r, BranchRoot &m) { m.root = readText(r); });
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

} // namespace distrib

bool isKnownDistribCode(quint32 code)
{
    if (code > 0xFF) {
        return false;
    }
    switch (static_cast<DistribCode>(code)) {
    case DistribCode::Ping:
    case DistribCode::Search:
    case DistribCode::BranchLevel:
    case DistribCode::BranchRoot:
    case DistribCode::ChildDepth:
    case DistribCode::EmbeddedMessage:
        return true;
    }
    return false;
}

Decoded<DistribMessage> decodeDistrib(quint32 code, QByteArrayView payload)
{
    using internal::decodeAs;
    using V = DistribMessage;
    if (code <= 0xFF) {
        switch (static_cast<DistribCode>(code)) {
        case DistribCode::Search:
            return decodeAs<V, distrib::Search>(payload);
        case DistribCode::BranchLevel:
            return decodeAs<V, distrib::BranchLevel>(payload);
        case DistribCode::BranchRoot:
            return decodeAs<V, distrib::BranchRoot>(payload);
        case DistribCode::EmbeddedMessage:
            return decodeAs<V, distrib::EmbeddedMessage>(payload);
        case DistribCode::Ping:
        case DistribCode::ChildDepth:
            return {DecodeStatus::Ignored, std::nullopt};
        }
    }
    return {DecodeStatus::Unknown, std::nullopt};
}

} // namespace selecta::slsk
