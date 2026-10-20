// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCryptographicHash>
#include <QtGlobal>

namespace selecta::convert::detail {

// Adds `count` float samples to `hash` as 32-bit little-endian values: the byte stream pcmDigest() hashes.
void addFloatSamples(QCryptographicHash &hash, const float *samples, qint64 count);

} // namespace selecta::convert::detail
