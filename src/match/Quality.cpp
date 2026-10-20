// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Quality.h"

#include <QString>

#include <array>
#include <utility>

namespace selecta::match {

namespace {

constexpr double kPlausibleShare = 0.85;  // the size may imply less than the declared bitrate by 15 % at most
constexpr int kLosslessMinimumKbps = 192; // below this a "lossless" file is implausible
constexpr int kMp3ExtremeKbps = 315;      // 320 CBR, with some slack for rounding
constexpr int kMp3HighKbps = 256;
constexpr int kMp3MediumKbps = 192;
constexpr int kVbrV0Kbps = 220;
constexpr int kVbrV2Kbps = 170;
constexpr int kTransparentKbps = 256; // AAC, Vorbis, Opus
constexpr int kGoodKbps = 192;
constexpr int kFairKbps = 128;

int lossyRank(Codec codec, int bitrateKbps, std::optional<bool> vbr)
{
    if (bitrateKbps <= 0) {
        return kQualityUnknown;
    }
    if (codec == Codec::Mp3) {
        if (vbr.value_or(false)) {
            if (bitrateKbps >= kVbrV0Kbps) {
                return 3;
            }
            return bitrateKbps >= kVbrV2Kbps ? 2 : 1;
        }
        if (bitrateKbps >= kMp3ExtremeKbps) {
            return 4;
        }
        if (bitrateKbps >= kMp3HighKbps) {
            return 3;
        }
        return bitrateKbps >= kMp3MediumKbps ? 2 : 1;
    }
    if (bitrateKbps >= kTransparentKbps) {
        return 4;
    }
    if (bitrateKbps >= kGoodKbps) {
        return 3;
    }
    return bitrateKbps >= kFairKbps ? 2 : 1;
}

} // namespace

Codec codecFromExtension(QStringView extension)
{
    static constexpr std::array<std::pair<QStringView, Codec>, 15> kExtensions{{
        {u"mp3", Codec::Mp3},
        {u"m4a", Codec::Aac},
        {u"aac", Codec::Aac},
        {u"mp4", Codec::Aac},
        {u"ogg", Codec::Vorbis},
        {u"oga", Codec::Vorbis},
        {u"opus", Codec::Opus},
        {u"wma", Codec::Wma},
        {u"flac", Codec::Flac},
        {u"alac", Codec::Alac},
        {u"wav", Codec::Wav},
        {u"aif", Codec::Aiff},
        {u"aiff", Codec::Aiff},
        {u"ape", Codec::Ape},
        {u"wv", Codec::WavPack},
    }};
    const QString lower = extension.toString().toLower();
    for (const auto &[name, codec] : kExtensions) {
        if (name == lower) {
            return codec;
        }
    }
    return Codec::Unknown;
}

Codec codecFor(QStringView extension, const RemoteAttributes &attributes)
{
    const Codec codec = codecFromExtension(extension);
    if (codec == Codec::Aac && attributes.bitDepth > 0) {
        return Codec::Alac; // .m4a with a bit depth: Apple Lossless
    }
    return codec;
}

bool isLossless(Codec codec)
{
    switch (codec) {
    case Codec::Flac:
    case Codec::Alac:
    case Codec::Wav:
    case Codec::Aiff:
    case Codec::Ape:
    case Codec::WavPack:
        return true;
    case Codec::Unknown:
    case Codec::Mp3:
    case Codec::Aac:
    case Codec::Vorbis:
    case Codec::Opus:
    case Codec::Wma:
        return false;
    }
    return false;
}

int qualityRank(const AudioQuality &quality)
{
    if (quality.codec == Codec::Unknown) {
        return kQualityUnknown;
    }
    if (isLossless(quality.codec)) {
        return quality.bitDepth >= 24 ? kQualityLossless24 : kQualityLossless16;
    }
    return lossyRank(quality.codec, quality.bitrateKbps, quality.vbr);
}

RemoteQuality assessRemoteQuality(QStringView extension, quint64 sizeBytes, const RemoteAttributes &attributes)
{
    RemoteQuality result;
    result.quality.codec = codecFor(extension, attributes);
    result.quality.bitrateKbps = attributes.bitrateKbps;
    result.quality.vbr = attributes.vbr;
    result.quality.sampleRateHz = attributes.sampleRateHz;
    result.quality.bitDepth = attributes.bitDepth;
    if (attributes.durationSec > 0 && sizeBytes > 0) {
        result.estimatedBitrateKbps =
            static_cast<int>((static_cast<double>(sizeBytes) * 8.0) / (attributes.durationSec * 1000.0));
    }
    if (result.quality.codec == Codec::Unknown) {
        return result;
    }
    const bool hasLength = attributes.durationSec > 0;
    if (isLossless(result.quality.codec)) {
        result.rank = qualityRank(result.quality);
        result.plausible = !hasLength || result.estimatedBitrateKbps >= kLosslessMinimumKbps;
        result.established = hasLength && result.plausible;
        return result;
    }
    if (attributes.bitrateKbps > 0) {
        result.rank = qualityRank(result.quality);
        result.plausible = !hasLength || result.estimatedBitrateKbps >= kPlausibleShare * attributes.bitrateKbps;
        result.established = hasLength && result.plausible;
        return result;
    }
    // Nothing declared: an estimate ranks the file (labelled "estimated" by the UI) but establishes nothing.
    AudioQuality estimated = result.quality;
    estimated.bitrateKbps = result.estimatedBitrateKbps;
    result.rank = qualityRank(estimated);
    return result;
}

} // namespace selecta::match
