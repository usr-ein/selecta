// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: peer-init messages (PeerInit, PierceFireWall), the first frame on every peer socket.
#include "FuzzCommon.h"
#include "slsk/PeerMessages.h"

using namespace selecta::slsk;

namespace {

void decodeOne(quint32 code, QByteArrayView payload)
{
    const Decoded<PeerInitMessage> decoded = decodePeerInit(code, payload);
    fuzz::checkStable(decoded);
    if (decoded.ok()) {
        if (const auto *init = std::get_if<peerinit::PeerInit>(&*decoded.message)) {
            (void)isAcceptablePeerUsername(init->username);
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArrayView input = fuzz::view(data, size);
    fuzz::forEachFrame(input, CodeWidth::Byte, protocol::kPeerInitFrameMax,
                       [](const Frame &frame) { decodeOne(frame.code, frame.payload); });
    if (!input.isEmpty()) {
        decodeOne(static_cast<quint8>(input.front()) % 4, input.sliced(1));
    }
    return 0;
}
