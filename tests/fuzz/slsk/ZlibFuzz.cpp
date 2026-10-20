// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: capped zlib inflate and prefix inflate.
#include "FuzzCommon.h"
#include "slsk/Zlib.h"

using namespace selecta::slsk;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArrayView input = fuzz::view(data, size);
    constexpr qsizetype kCap = protocol::kMiB;
    const zlib::Inflated inflated = zlib::inflate(input, kCap);
    if (inflated.ok()) {
        if (inflated.data.size() > kCap) {
            fuzz::fail("inflate produced more than its cap");
        }
        const zlib::Inflated again = zlib::inflate(zlib::deflate(inflated.data), kCap);
        if (!again.ok() || again.data != inflated.data) {
            fuzz::fail("deflate/inflate round trip differs");
        }
    } else if (!inflated.data.isEmpty()) {
        fuzz::fail("failed inflate returned data");
    }
    const zlib::Inflated prefix = zlib::inflatePrefix(input, 264);
    if (prefix.ok() && prefix.data.size() > 264) {
        fuzz::fail("prefix longer than asked");
    }
    if (inflated.ok() && prefix.ok() && !inflated.data.startsWith(prefix.data)) {
        fuzz::fail("prefix differs from the start of the full stream");
    }
    return 0;
}
