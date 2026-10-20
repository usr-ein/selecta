// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Codec.h"
#include "slsk/Frame.h"

#include <QByteArray>
#include <QByteArrayView>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <variant>

// Shared helpers for the slsk fuzz targets. A target aborts (a "crash" for libFuzzer) when the codec misbehaves in a
// way the sanitizers can't see: a message that decodes but doesn't survive re-encoding.
namespace selecta::slsk::fuzz {

[[noreturn]] inline void fail(const char *what)
{
    (void)std::fputs(what, stderr);
    (void)std::fputc('\n', stderr);
    std::abort();
}

inline QByteArrayView view(const uint8_t *data, size_t size)
{
    return {reinterpret_cast<const char *>(data), static_cast<qsizetype>(size)};
}

// Decoded → encode → decode must give the same message back.
template <typename Variant>
void checkStable(const Decoded<Variant> &decoded)
{
    if (!decoded.ok()) {
        return;
    }
    std::visit(
        [](const auto &message) {
            using M = std::decay_t<decltype(message)>;
            const std::optional<M> again = M::decode(message.encode());
            if (!again) {
                fail("re-encoded message no longer decodes");
            }
            if (!(*again == message)) {
                fail("re-encoded message decodes differently");
            }
        },
        *decoded.message);
}

// Feeds `input` to a frame reader in two chunks (split where the first byte says) and hands each frame to `onFrame`.
template <typename OnFrame>
void forEachFrame(QByteArrayView input, CodeWidth width, qsizetype cap, const OnFrame &onFrame)
{
    FrameReader reader(width, cap);
    const qsizetype split = input.isEmpty() ? 0 : static_cast<quint8>(input.front()) % (input.size() + 1);
    for (const QByteArrayView chunk : {input.first(split), input.sliced(split)}) {
        reader.append(chunk);
        while (const std::optional<Frame> frame = reader.next()) {
            onFrame(*frame);
        }
    }
}

} // namespace selecta::slsk::fuzz
