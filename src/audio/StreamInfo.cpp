// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/StreamInfo.h"

using namespace Qt::StringLiterals;

namespace selecta::audio {

QString containerName(Container container)
{
    switch (container) {
    case Container::Unknown:
        return u"Unknown"_s;
    case Container::MpegAudio:
        return u"MP3"_s;
    case Container::Flac:
        return u"FLAC"_s;
    case Container::Wav:
        return u"WAV"_s;
    case Container::Aiff:
        return u"AIFF"_s;
    case Container::Mp4:
        return u"MP4"_s;
    case Container::Ogg:
        return u"Ogg"_s;
    case Container::Other:
        return u"Other"_s;
    }
    return u"Unknown"_s;
}

QString codecName(Codec codec)
{
    switch (codec) {
    case Codec::Unknown:
        return u"Unknown"_s;
    case Codec::Mp3:
        return u"MP3"_s;
    case Codec::Mp2:
        return u"MP2"_s;
    case Codec::Mp1:
        return u"MP1"_s;
    case Codec::Flac:
        return u"FLAC"_s;
    case Codec::Pcm:
        return u"PCM"_s;
    case Codec::Aac:
        return u"AAC"_s;
    case Codec::Alac:
        return u"ALAC"_s;
    case Codec::Vorbis:
        return u"Vorbis"_s;
    case Codec::Opus:
        return u"Opus"_s;
    case Codec::Other:
        return u"Other"_s;
    }
    return u"Unknown"_s;
}

} // namespace selecta::audio
