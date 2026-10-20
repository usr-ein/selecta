// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <optional>

// Spotify links and URIs as users paste them (META-3, references/metadata-and-preview-services.md §14.6):
//   https://open.spotify.com/track/<id>, with or without an intl-xx/ segment and ?si=… or other query parameters;
//   spotify:track:<id> (copied with ⌥ in the Spotify app);
// for tracks, albums and playlists. IDs are 22 base-62 characters. Shortened spotify.link/… links aren't followed:
// they would need a request to resolve, so they count as skipped. Pure functions, thread-safe.
namespace selecta::meta::spotify {

enum class LinkKind : quint8 {
    Track,
    Album,
    Playlist,
};

struct Link
{
    LinkKind kind = LinkKind::Track;
    QString id;

    bool operator==(const Link &) const = default;
};

inline constexpr int kIdLength = 22;
inline constexpr int kMaxPastedLines = 10'000; // lines beyond this are counted as skipped, not parsed

bool isValidId(QStringView id);

// One link or URI (surrounding whitespace ignored), or nullopt when it isn't a track, album or playlist link.
std::optional<Link> parseLink(QStringView text);

// The canonical web URL for a link: https://open.spotify.com/<kind>/<id>.
QString webUrl(const Link &link);

struct PastedLinks
{
    QList<Link> links;  // in the order pasted, duplicates removed
    int duplicates = 0; // repeated links dropped
    int skipped = 0;    // non-empty lines that aren't links (or are beyond kMaxPastedLines)
};

// Every link in pasted text: one per line, or several separated by whitespace on a line (the Spotify app copies
// newline-separated URLs; some apps separate with spaces).
PastedLinks parsePastedLinks(QStringView text);

} // namespace selecta::meta::spotify
