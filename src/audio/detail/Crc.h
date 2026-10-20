// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/detail/Bytes.h"

#include <QtGlobal>

namespace selecta::audio::detail {

// CRC-8, polynomial 0x07, initial 0: FLAC frame headers.
quint8 crc8Flac(ByteSpan bytes);
// CRC-16, polynomial 0x8005, not reflected, initial 0: FLAC frame footers.
quint16 crc16Flac(ByteSpan bytes, quint16 initial = 0);
// CRC-16/ARC, polynomial 0x8005 reflected (0xA001), initial 0: the LAME tag CRC (first 190 bytes of the tag frame) and
// the "music CRC".
quint16 crc16Lame(ByteSpan bytes, quint16 initial = 0);

} // namespace selecta::audio::detail
