// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/ServicePolicy.h"

namespace selecta::meta {

const ServicePolicy &policyFor(Service service)
{
    switch (service) {
    case Service::SpotifyApi:
        return kSpotifyApiPolicy;
    case Service::SpotifyAccounts:
        return kSpotifyAccountsPolicy;
    case Service::SpotifyImages:
        return kSpotifyImagesPolicy;
    case Service::Deezer:
        return kDeezerPolicy;
    case Service::MusicBrainz:
        return kMusicBrainzPolicy;
    case Service::CoverArtArchive:
        return kCoverArtArchivePolicy;
    case Service::YtDlp:
    case Service::Keychain: // not a network service; the strictest pacing if it ever got here
        return kYtDlpPolicy;
    }
    return kYtDlpPolicy;
}

} // namespace selecta::meta
