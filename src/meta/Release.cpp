// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/Release.h"

using namespace Qt::StringLiterals;

namespace selecta::meta {

QString sourceName(Source source)
{
    switch (source) {
    case Source::Spotify:
        return u"Spotify"_s;
    case Source::Deezer:
        return u"Deezer"_s;
    case Source::MusicBrainz:
        return u"MusicBrainz"_s;
    case Source::CoverArtArchive:
        return u"Cover Art Archive"_s;
    case Source::YouTube:
        return u"YouTube"_s;
    }
    return u"unknown source"_s;
}

} // namespace selecta::meta
