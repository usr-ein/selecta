// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Release.h"
#include "meta/SpotifyClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Spotify Web API objects → meta's value types. Every field is optional and type-checked; nothing is invented (a
// missing ISRC stays empty, a missing total stays unset). Pure functions, thread-safe.
namespace selecta::meta::detail::spotify {

// Images, largest first; entries without a usable https URL are dropped; at most json::kMaxListItems.
QList<CoverRef> images(const QJsonArray &array);

// A full or simplified track object. Album fields are filled when the track carries its album.
Release track(const QJsonObject &object);

// An album object, its first page of tracks included (album-level fields copied into each track). The caller adds
// further pages with appendAlbumTracks() and then calls finishAlbum().
Album album(const QJsonObject &object);
void appendAlbumTracks(Album &album, const QJsonArray &items);
// Computes the disc total from the tracks and copies it, and the album fields, into every track.
void finishAlbum(Album &album);

PlaylistSummary playlist(const QJsonObject &object);

// The track inside a playlist or library item ({"track": …} or {"item": …}); nullopt for episodes and unreadable
// items. Local files (is_local) come back without a Spotify ID.
std::optional<Release> itemTrack(const QJsonObject &item);

// The Web API paths SpotifyClient calls, all among the endpoints development mode still serves (February 2026
// list, references/metadata-and-preview-services.md §3.3 and §4.2). `path` is relative to /v1.
bool isDevelopmentModeEndpoint(const QString &path);

} // namespace selecta::meta::detail::spotify
