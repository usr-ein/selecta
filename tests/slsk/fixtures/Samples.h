// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/DistributedMessages.h"
#include "slsk/PeerMessages.h"
#include "slsk/ServerMessages.h"

#include <QList>

// One or more filled-in instances of every wire message, for round-trip, truncation and garbage tests and as fuzz
// seeds. Values are deliberately awkward: non-ASCII names, raw Latin-1 paths, 64-bit sizes, empty and maximal fields.
namespace selecta::slsk::testing {

FileEntry sampleFile(int index);
SharedDirectory sampleDirectory(int index, int files);
SharedFileList sampleShareList(int directories, int filesPerDirectory);

QList<ToServerMessage> toServerSamples();
QList<FromServerMessage> fromServerSamples();
QList<PeerInitMessage> peerInitSamples();
QList<PeerMessage> peerSamples();
QList<DistribMessage> distribSamples();

// The framed bytes of a sample, whatever its type.
template <typename Variant>
QByteArray frameOf(const Variant &message)
{
    return std::visit([](const auto &m) { return frame(m); }, message);
}

template <typename Variant>
quint32 codeOf(const Variant &message)
{
    return std::visit([](const auto &m) { return static_cast<quint32>(std::decay_t<decltype(m)>::kCode); }, message);
}

template <typename Variant>
QByteArray payloadOf(const Variant &message)
{
    return std::visit([](const auto &m) { return m.encode(); }, message);
}

} // namespace selecta::slsk::testing
