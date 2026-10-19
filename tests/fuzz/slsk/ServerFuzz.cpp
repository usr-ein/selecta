// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: the server channel, both directions (decodeFromServer, decodeToServer).
#include "FuzzCommon.h"
#include "slsk/ServerMessages.h"

#include <QtEndian>

using namespace selecta::slsk;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArrayView input = fuzz::view(data, size);
    // As a framed stream under the server cap.
    fuzz::forEachFrame(input, CodeWidth::Word, protocol::kServerFrameMax, [](const Frame &frame) {
        fuzz::checkStable(decodeFromServer(frame.code, frame.payload));
        fuzz::checkStable(decodeToServer(frame.code, frame.payload));
    });
    // As one unframed payload: a 32-bit code, then the payload.
    if (input.size() >= 4) {
        const auto code = qFromLittleEndian<quint32>(input.data());
        fuzz::checkStable(decodeFromServer(code, input.sliced(4)));
        fuzz::checkStable(decodeToServer(code, input.sliced(4)));
    }
    return 0;
}
