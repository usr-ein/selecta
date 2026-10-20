// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: peer (`P`) messages, including the zlib-compressed share lists, search results and folder contents, and
// the raw `F` handshake.
#include "FuzzCommon.h"
#include "slsk/PeerMessages.h"

#include <QtEndian>

using namespace selecta::slsk;

namespace {

// Smaller than the production caps, so a zip bomb can't exhaust the fuzzer's memory limit; the code paths are the same.
constexpr qsizetype kInflateCap = 8 * protocol::kMiB;

template <typename T>
void checkCompressed(QByteArrayView payload)
{
    const std::optional<T> message = T::decode(payload, kInflateCap);
    if (!message) {
        return;
    }
    const std::optional<T> again = T::decode(message->encode(), kInflateCap);
    if (!again || !(*again == *message)) {
        fuzz::fail("compressed peer message does not survive re-encoding");
    }
}

void decodeOne(quint32 code, QByteArrayView payload)
{
    switch (static_cast<PeerCode>(code)) {
    case PeerCode::SharedFileListResponse:
        checkCompressed<peer::SharedFileListResponse>(payload);
        return;
    case PeerCode::FileSearchResponse:
        (void)peer::FileSearchResponse::peek(payload);
        checkCompressed<peer::FileSearchResponse>(payload);
        return;
    case PeerCode::FolderContentsResponse:
        checkCompressed<peer::FolderContentsResponse>(payload);
        return;
    default:
        fuzz::checkStable(decodePeer(code, payload));
        return;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArrayView input = fuzz::view(data, size);
    fuzz::forEachFrame(input, CodeWidth::Word, protocol::kPeerLargeFrameMax,
                       [](const Frame &frame) { decodeOne(frame.code, frame.payload); });
    if (input.size() >= 4) {
        decodeOne(qFromLittleEndian<quint32>(input.data()), input.sliced(4));
    }
    // The raw file-connection handshake.
    (void)file::decodeTransferInit(input.first(std::min<qsizetype>(input.size(), file::kTransferInitBytes)));
    (void)file::decodeOffset(input.first(std::min<qsizetype>(input.size(), file::kOffsetBytes)));
    return 0;
}
