// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: distributed (`D`) messages, including searches embedded in EmbeddedMessage.
#include "FuzzCommon.h"
#include "slsk/DistributedMessages.h"

using namespace selecta::slsk;

namespace {

void decodeOne(quint32 code, QByteArrayView payload)
{
    const Decoded<DistribMessage> decoded = decodeDistrib(code, payload);
    fuzz::checkStable(decoded);
    if (decoded.ok()) {
        if (const auto *embedded = std::get_if<distrib::EmbeddedMessage>(&*decoded.message)) {
            fuzz::checkStable(decodeDistrib(embedded->distribCode, embedded->payload));
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArrayView input = fuzz::view(data, size);
    fuzz::forEachFrame(input, CodeWidth::Byte, protocol::kDistribFrameMax,
                       [](const Frame &frame) { decodeOne(frame.code, frame.payload); });
    if (!input.isEmpty()) {
        decodeOne(static_cast<quint8>(input.front()), input.sliced(1));
    }
    return 0;
}
