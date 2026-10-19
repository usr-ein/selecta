// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QStringView>

namespace selecta::match {

// Quality ranks (note §9), high to low: lossless ≥ 24-bit = 6, lossless 16-bit = 5, MP3 320 CBR or AAC/Vorbis/Opus
// ≥ 256 = 4, MP3 V0-class VBR (≥ 220 average) or 256 CBR = 3, V2-class VBR (≥ 170) or 192 = 2, below = 1. 0 means
// the quality is unknown and never takes part in a comparison.
inline constexpr int kQualityUnknown = 0;
inline constexpr int kQualityLossless24 = 6;
inline constexpr int kQualityLossless16 = 5;

// The codec of a file name extension, case-insensitive ("flac", "MP3", "m4a" → AAC: ALAC in an .m4a needs a bit
// depth to tell, see codecFor()). Unknown extensions give Codec::Unknown.
Codec codecFromExtension(QStringView extension);

// The codec of a remote file: its extension, refined by its attributes (an .m4a with a bit depth is ALAC).
Codec codecFor(QStringView extension, const RemoteAttributes &attributes);

bool isLossless(Codec codec);

// The rank of known audio properties (local files: the scanner's TagLib values). 0 when the bitrate of a lossy file
// is unknown. Lossless without a bit depth counts as 16-bit.
int qualityRank(const AudioQuality &quality);

// What a search result's attributes establish about its quality (PRD SRCH-6, SRCH-7).
struct RemoteQuality
{
    AudioQuality quality;         // codec and attributes as declared
    int rank = kQualityUnknown;   // from the declared values, or from the estimate when nothing is declared
    int estimatedBitrateKbps = 0; // size × 8 / length; 0 without a length
    bool established = false;     // declared attributes are present and plausible: may claim an upgrade (LIB-4)
    bool plausible = true;        // false when the size contradicts the declared bitrate or format (SRCH-7)
};

// Assesses a remote file. Lossy: established when a bitrate and a length are declared and the size implies at least
// 85 % of the declared bitrate (tags and artwork only add bytes). Lossless: established when a length is declared and
// the size implies at least 192 kbps. Without a length nothing can be checked, so nothing is established. Pure; any
// thread.
RemoteQuality assessRemoteQuality(QStringView extension, quint64 sizeBytes, const RemoteAttributes &attributes);

} // namespace selecta::match
